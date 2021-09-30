import argparse
import time
import torch
import torch.nn as nn
import torch.nn.functional as F
import torch.optim as optim
import numpy as np
import dgl.function as fn
from dgl.nn.pytorch import GraphConv
import dgl.multiprocessing as mp
from torch.nn.parallel import DistributedDataParallel
import sys
import os
from enum import Enum
'''
os.environ['CUDA_VISIBLE_DEVICES'] = '0,1,2'
os.environ["NCCL_DEBUG"] = "INFO"
'''
import samgraph.torch as sam
from common_config import *

"""
  We have made the following modification(or say, simplification) on PinSAGE,
  because we only want to focus on the core algorithm of PinSAGE:
    1. we modify PinSAGE to make it be able to be trained on homogenous graph.
    2. we use cross-entropy loss instead of max-margin ranking loss describe in the paper.
"""


class WeightedSAGEConv(nn.Module):
    def __init__(self, input_dims, hidden_dims, output_dims, dropout, act=F.relu):
        super().__init__()

        self.act = act
        self.Q = nn.Linear(input_dims, hidden_dims)
        self.W = nn.Linear(input_dims + hidden_dims, output_dims)
        self.reset_parameters()
        self.dropout = nn.Dropout(dropout)

    def reset_parameters(self):
        gain = nn.init.calculate_gain('relu')
        nn.init.xavier_uniform_(self.Q.weight, gain=gain)
        nn.init.xavier_uniform_(self.W.weight, gain=gain)
        nn.init.constant_(self.Q.bias, 0)
        nn.init.constant_(self.W.bias, 0)

    def forward(self, g, h, weights):
        """
        g : graph
        h : node features
        weights : scalar edge weights
        """
        h_src, h_dst = h
        with g.local_scope():
            g.srcdata['n'] = self.act(self.Q(self.dropout(h_src)))
            g.edata['w'] = weights.float()
            g.update_all(fn.u_mul_e('n', 'w', 'm'), fn.sum('m', 'n'))
            g.update_all(fn.copy_e('w', 'm'), fn.sum('m', 'ws'))
            n = g.dstdata['n']
            ws = g.dstdata['ws'].unsqueeze(1).clamp(min=1)
            z = self.act(self.W(self.dropout(torch.cat([n / ws, h_dst], 1))))
            z_norm = z.norm(2, 1, keepdim=True)
            z_norm = torch.where(
                z_norm == 0, torch.tensor(1.).to(z_norm), z_norm)
            z = z / z_norm
            return z


class PinSAGE(nn.Module):
    def __init__(self,
                 in_feats,
                 n_hidden,
                 n_classes,
                 n_layers,
                 activation,
                 dropout):
        super().__init__()
        self.n_layers = n_layers
        self.n_hidden = n_hidden
        self.n_classes = n_classes
        self.layers = nn.ModuleList()

        self.layers.append(WeightedSAGEConv(
            in_feats, n_hidden, n_hidden, dropout, activation))
        for _ in range(1, n_layers - 1):
            self.layers.append(WeightedSAGEConv(
                n_hidden, n_hidden, n_hidden, dropout, activation))
        self.layers.append(WeightedSAGEConv(
            n_hidden, n_hidden, n_classes, dropout, activation))

    def forward(self, blocks, h):
        for layer, block in zip(self.layers, blocks):
            h_dst = h[:block.number_of_nodes('DST/' + block.ntypes[0])]
            h = layer(block, (h, h_dst), block.edata['weights'])
        return h


def parse_args(default_run_config):
    argparser = argparse.ArgumentParser("PinSAGE Training")

    add_common_arguments(argparser, default_run_config)

    argparser.add_argument('--random-walk-length', type=int,
                           default=default_run_config['random_walk_length'])
    argparser.add_argument('--random-walk-restart-prob',
                           type=float, default=default_run_config['random_walk_restart_prob'])
    argparser.add_argument('--num-random-walk', type=int,
                           default=default_run_config['num_random_walk'])
    argparser.add_argument('--num-neighbor', type=int,
                           default=default_run_config['num_neighbor'])
    argparser.add_argument('--num-layer', type=int,
                           default=default_run_config['num_layer'])

    argparser.add_argument(
        '--lr', type=float, default=default_run_config['lr'])
    argparser.add_argument('--dropout', type=float,
                           default=default_run_config['dropout'])

    return vars(argparser.parse_args())


def get_run_config():
    run_config = {}

    run_config.update(get_default_common_config(run_multi_gpu=True))
    run_config['sample_type'] = 'random_walk'

    run_config['random_walk_length'] = 4
    run_config['random_walk_restart_prob'] = 0.5
    run_config['num_random_walk'] = 4
    run_config['num_neighbor'] = 8
    run_config['num_layer'] = 3

    run_config['lr'] = 0.003
    run_config['dropout'] = 0.5

    run_config.update(parse_args(run_config))

    process_common_config(run_config)
    assert(run_config['arch'] == 'arch5')
    assert(run_config['sample_type'] == 'random_walk')

    print_run_config(run_config)

    if run_config['validate_configs']:
        sys.exit()

    return run_config


def run_init(run_config):
    sam.config(run_config)
    sam.data_init()

    if run_config['validate_configs']:
        sys.exit()


def run_sample(worker_id, run_config):
    # os.environ['CUDA_VISIBLE_DEVICES'] = '0'
    num_worker = run_config['num_sample_worker']
    global_barrier = run_config['global_barrier']

    # used for message counter
    mq_sem  = config['mq_sem']
    sampler_stop_event = config['sampler_stop_event']

    ctx = run_config['sample_workers'][worker_id]
    print('sample device: ', torch.cuda.get_device_name(ctx))

    print('[Sample Worker {:d}/{:d}] Started with PID {:d}'.format(
        worker_id, num_worker, os.getpid()))
    sam.sample_init(worker_id, ctx)
    sam.notify_sampler_ready(global_barrier)

    num_epoch = sam.num_epoch()
    num_step = sam.steps_per_epoch()
    if (worker_id == (num_worker - 1)):
        num_step = int(num_step - int(num_step /
                       num_worker * worker_id))
    else:
        num_step = int(num_step / num_worker)

    epoch_sample_total_times_python = []
    epoch_sample_total_times_profiler = []
    epoch_sample_times = []
    epoch_get_cache_miss_index_times = []
    epoch_buffer_graph_times = []

    print('[Sample Worker {:d}] run sample for {:d} epochs with {:d} steps'.format(
        worker_id, num_epoch, num_step))

    # run start barrier
    global_barrier.wait()

    for epoch in range(num_epoch):
        # epoch start barrier 1
        sampler_stop_event.clear()
        global_barrier.wait()

        tic = time.time()
        for step in range(num_step):
            # print(f'sample epoch {epoch}, step {step}')
            sam.sample_once()
            sam.report_step(epoch, step)
            # let counter of message queue increase one
            mq_sem.release()

        # notify switcher that sampler succeed
        sampler_stop_event.set()
        toc = time.time()

        epoch_sample_total_times_python.append(toc - tic)
        epoch_sample_times.append(
            sam.get_log_epoch_value(epoch, sam.kLogEpochSampleTime))
        epoch_get_cache_miss_index_times.append(
            sam.get_log_epoch_value(
                epoch, sam.KLogEpochSampleGetCacheMissIndexTime)
        )
        epoch_buffer_graph_times.append(
            sam.get_log_epoch_value(epoch, sam.kLogEpochSampleSendTime)
        )
        epoch_sample_total_times_profiler.append(
            sam.get_log_epoch_value(epoch, sam.kLogEpochSampleTotalTime)
        )

        # epoch end barrier
        global_barrier.wait()

    print('[Sample Worker {:d}] Avg Sample Total Time {:.4f} | Sampler Total Time(Profiler) {:.4f}'.format(
        worker_id, np.mean(epoch_sample_total_times_python[1:]), np.mean(epoch_sample_total_times_profiler[1:])))

    if worker_id == 0:
        sam.report_step_average(epoch - 1, step - 1)

    # run end barrier
    global_barrier.wait()

    if worker_id == 0:
        test_result = {}

        test_result['sample_time'] = np.mean(epoch_sample_times[1:])
        test_result['cache_index_time'] = np.mean(
            epoch_get_cache_miss_index_times[1:])
        test_result['buffer_graph_time'] = np.mean(
            epoch_buffer_graph_times[1:])
        test_result['epoch_time:sample_total'] = np.mean(
            epoch_sample_total_times_python[1:])
        for k, v in test_result.items():
            print('test_result:{:}={:.2f}'.format(k, v))

    sam.shutdown()

class TrainerType(Enum):
    Trainer = 1
    Switcher = 2

def run_train(worker_id, run_config, trainer_type):
    ctx = None
    if (trainer_type == TrainerType.Trainer):
        ctx = run_config['train_workers'][worker_id]
    elif (trainer_type == TrainerType.Switcher):
        ctx = run_config['sample_workers'][worker_id]

    # used for sync
    global_barrier = run_config['global_barrier']
    mq_sem  = config['mq_sem']
    sampler_stop_event = config['sampler_stop_event']

    train_device = torch.device(ctx)
    print('[{:s}     Worker  {:d}/{:d}] Started with PID {:d} Train Device '.format(
        trainer_type.name, worker_id, run_config['num_train_worker'], os.getpid()), train_device)


    # let the trainer initialization after sampler
    # sampler should presample before trainer initialization
    sam.wait_for_sampler_ready(global_barrier)
    # TODO: need to change the cache_percentage ???
    sam.train_init(worker_id, ctx)

    in_feat = sam.feat_dim()
    num_class = sam.num_class()
    num_layer = run_config['num_layer']

    model = PinSAGE(in_feat, run_config['num_hidden'], num_class,
                    num_layer, F.relu, run_config['dropout'])
    model = model.to(train_device)

    loss_fcn = nn.CrossEntropyLoss()
    loss_fcn = loss_fcn.to(train_device)
    optimizer = optim.Adam(
        model.parameters(), lr=run_config['lr'])

    num_epoch = sam.num_epoch()

    model.train()

    epoch_copy_times = []
    epoch_convert_times = []
    epoch_train_times = []
    epoch_total_times_python = []
    epoch_train_total_times_profiler = []
    epoch_cache_hit_rates = []

    copy_times = []
    convert_times = []
    train_times = []
    total_times = []

    # run start barrier
    global_barrier.wait()
    print('[Train  Worker {:d}] run train for {:d} epochs with global {:d} steps'.format(
        worker_id, num_epoch, num_step))

    for epoch in range(num_epoch):
        tic = time.time()

        # epoch start barrier
        global_barrier.wait()

        if (trainer_type == TrainerType.Switcher):
            sampler_stop_event.wait()

        while True:
            if (not mq_sem.acquire(timeout=0.01)):
                if (trainer_type == TrainerType.Switcher):
                    break
                elif (trainer_type == TrainerType.Trainer):
                    if (sampler_stop_event.is_set()):
                        break
                    else: # expecially for the first step of trainer
                        continue
                else:
                    assert(0)
            t0 = time.time()
            sam.sample_once()
            batch_key = sam.get_next_batch()
            t1 = time.time()
            blocks, batch_input, batch_label = sam.get_dgl_blocks_with_weights(
                batch_key, num_layer)
            t2 = time.time()

            # Compute loss and prediction
            batch_pred = model(blocks, batch_input)
            loss = loss_fcn(batch_pred, batch_label)
            optimizer.zero_grad()
            loss.backward()
            optimizer.step()

            batch_input = None
            batch_label = None

            # sync the arguments
            # bala bala ...

            t3 = time.time()

            copy_time = sam.get_log_step_value(epoch, step, sam.kLogL1CopyTime)
            convert_time = t2 - t1
            train_time = t3 - t2
            total_time = t3 - t1

            sam.log_step(epoch, step, sam.kLogL1TrainTime, train_time)
            sam.log_step(epoch, step, sam.kLogL1ConvertTime, convert_time)
            sam.log_epoch_add(epoch, sam.kLogEpochConvertTime, convert_time)
            sam.log_epoch_add(epoch, sam.kLogEpochTrainTime, train_time)
            sam.log_epoch_add(epoch, sam.kLogEpochTotalTime, total_time)

            feat_nbytes = sam.get_log_epoch_value(
                epoch, sam.kLogEpochFeatureBytes)
            miss_nbytes = sam.get_log_epoch_value(
                epoch, sam.kLogEpochMissBytes)
            epoch_cache_hit_rates.append(
                (feat_nbytes - miss_nbytes) / feat_nbytes)

            copy_times.append(copy_time)
            convert_times.append(convert_time)
            train_times.append(train_time)
            total_times.append(total_time)

            sam.report_step(epoch, step)

        # sync the train workers
        toc = time.time()

        epoch_total_times_python.append(toc - tic)

        # epoch end barrier
        global_barrier.wait()

        epoch_copy_times.append(
            sam.get_log_epoch_value(epoch, sam.kLogEpochCopyTime))
        epoch_convert_times.append(
            sam.get_log_epoch_value(epoch, sam.kLogEpochConvertTime))
        epoch_train_times.append(
            sam.get_log_epoch_value(epoch, sam.kLogEpochTrainTime))
        epoch_train_total_times_profiler.append(
            sam.get_log_epoch_value(epoch, sam.kLogEpochTotalTime))
        if worker_id == 0:
            print('Epoch {:05d} | Epoch Time {:.4f} | Total Train Time(Profiler) {:.4f} | Copy Time {:.4f}'.format(
                epoch, epoch_total_times_python[-1], epoch_train_total_times_profiler[-1], epoch_copy_times[-1]))

    print('[{:s}  Worker {:d}] Epoch Time {:.4f} | Train Total Time(Profiler) {:.4f} | Copy Time {:.4f}'.format(
      trainer_type.name, worker_id, np.mean(epoch_total_times_python[1:]), np.mean(epoch_train_total_times_profiler[1:]), np.mean(epoch_copy_times[1:])))

    # run end barrier
    global_barrier.wait()

    if worker_id == 0:
        test_result = {}
        test_result['epoch_time:copy_time'] = np.mean(epoch_copy_times[1:])
        test_result['convert_time'] = np.mean(epoch_convert_times[1:])
        test_result['train_time'] = np.mean(epoch_train_times[1:])
        test_result['epoch_time:train_total'] = np.mean(
            epoch_train_total_times_profiler[1:])
        test_result['cache_percentage'] = run_config['cache_percentage']
        test_result['cache_hit_rate'] = np.mean(epoch_cache_hit_rates[1:])
        for k, v in test_result.items():
            print('test_result:{:}={:.2f}'.format(k, v))

    sam.shutdown()

def run_switcher(worker_id, run_config):
    ctx = run_config['sample_workers'][worker_id]
    num_worker = run_config['sample_workers']
    global_barrier = run_config['global_barrier']

    train_device = torch.device(ctx)
    print('[Train  Worker {:d}/{:d}] Started with PID {:d}'.format(
        worker_id, num_worker, os.getpid()))

    # let the trainer initialization after sampler
    # sampler should presample before trainer initialization
    sam.wait_for_sampler_ready(global_barrier)
    sam.train_init(worker_id, ctx)

    in_feat = sam.feat_dim()
    num_class = sam.num_class()
    num_layer = run_config['num_layer']

    model = PinSAGE(in_feat, run_config['num_hidden'], num_class,
                    num_layer, F.relu, run_config['dropout'])
    model = model.to(train_device)

    loss_fcn = nn.CrossEntropyLoss()
    loss_fcn = loss_fcn.to(train_device)
    optimizer = optim.Adam(
        model.parameters(), lr=run_config['lr'])

    num_epoch = sam.num_epoch()
    num_step = sam.steps_per_epoch()

    model.train()

    epoch_copy_times = []
    epoch_convert_times = []
    epoch_train_times = []
    epoch_total_times_python = []
    epoch_train_total_times_profiler = []
    epoch_cache_hit_rates = []

    copy_times = []
    convert_times = []
    train_times = []
    total_times = []

    # run start barrier
    global_barrier.wait()

    for epoch in range(num_epoch):
        # epoch start barrier
        global_barrier.wait()

        print('[Train  Switcher {:d}] run train for {:d} epochs with {:d} steps'.format(
            worker_id, num_epoch, num_step))

        tic = time.time()

        for step in range(worker_id, align_up_step, num_worker):
            if (step < num_step):
                t0 = time.time()
                sam.sample_once()
                batch_key = sam.get_next_batch()
                t1 = time.time()
                blocks, batch_input, batch_label = sam.get_dgl_blocks_with_weights(
                    batch_key, num_layer)
                t2 = time.time()
            else:
                t0 = t1 = t2 = time.time()

            # Compute loss and prediction
            batch_pred = model(blocks, batch_input)
            loss = loss_fcn(batch_pred, batch_label)
            optimizer.zero_grad()
            loss.backward()
            optimizer.step()

            if (step + num_worker < num_step):
                batch_input = None
                batch_label = None

            # sync the arguments
            # bala bala ...

            t3 = time.time()

            copy_time = sam.get_log_step_value(epoch, step, sam.kLogL1CopyTime)
            convert_time = t2 - t1
            train_time = t3 - t2
            total_time = t3 - t1

            sam.log_step(epoch, step, sam.kLogL1TrainTime, train_time)
            sam.log_step(epoch, step, sam.kLogL1ConvertTime, convert_time)
            sam.log_epoch_add(epoch, sam.kLogEpochConvertTime, convert_time)
            sam.log_epoch_add(epoch, sam.kLogEpochTrainTime, train_time)
            sam.log_epoch_add(epoch, sam.kLogEpochTotalTime, total_time)

            feat_nbytes = sam.get_log_epoch_value(
                epoch, sam.kLogEpochFeatureBytes)
            miss_nbytes = sam.get_log_epoch_value(
                epoch, sam.kLogEpochMissBytes)
            epoch_cache_hit_rates.append(
                (feat_nbytes - miss_nbytes) / feat_nbytes)

            copy_times.append(copy_time)
            convert_times.append(convert_time)
            train_times.append(train_time)
            total_times.append(total_time)

            sam.report_step(epoch, step)

        # sync the train workers
        toc = time.time()

        epoch_total_times_python.append(toc - tic)

        # epoch end barrier
        global_barrier.wait()

        epoch_copy_times.append(
            sam.get_log_epoch_value(epoch, sam.kLogEpochCopyTime))
        epoch_convert_times.append(
            sam.get_log_epoch_value(epoch, sam.kLogEpochConvertTime))
        epoch_train_times.append(
            sam.get_log_epoch_value(epoch, sam.kLogEpochTrainTime))
        epoch_train_total_times_profiler.append(
            sam.get_log_epoch_value(epoch, sam.kLogEpochTotalTime))
        if worker_id == 0:
            print('Epoch {:05d} | Epoch Time {:.4f} | Total Train Time(Profiler) {:.4f} | Copy Time {:.4f}'.format(
                epoch, epoch_total_times_python[-1], epoch_train_total_times_profiler[-1], epoch_copy_times[-1]))

    print('[Train  Worker {:d}] Epoch Time {:.4f} | Train Total Time(Profiler) {:.4f} | Copy Time {:.4f}'.format(
          worker_id, np.mean(epoch_total_times_python[1:]), np.mean(epoch_train_total_times_profiler[1:]), np.mean(epoch_copy_times[1:])))

    # run end barrier
    global_barrier.wait()

    if worker_id == 0:
        test_result = {}
        test_result['epoch_time:copy_time'] = np.mean(epoch_copy_times[1:])
        test_result['convert_time'] = np.mean(epoch_convert_times[1:])
        test_result['train_time'] = np.mean(epoch_train_times[1:])
        test_result['epoch_time:train_total'] = np.mean(
            epoch_train_total_times_profiler[1:])
        test_result['cache_percentage'] = run_config['cache_percentage']
        test_result['cache_hit_rate'] = np.mean(epoch_cache_hit_rates[1:])
        for k, v in test_result.items():
            print('test_result:{:}={:.2f}'.format(k, v))

    sam.shutdown()


if __name__ == '__main__':
    run_config = get_run_config()
    run_init(run_config)

    num_sample_worker = run_config['num_sample_worker']
    num_train_worker = run_config['num_train_worker']

    # global barrier is used to sync all the sample workers and train workers
    run_config['global_barrier'] = mp.Barrier(
        2 * num_sample_worker + num_train_worker, timeout=get_default_timeout())
    # let others know the info of message queue
    run_config['mq_sem'] = mp.Semaphore(0)
    # sampler_stop_event is used to notify the train_switcher
    run_config['sampler_stop_event'] = mp.Event()

    workers = []
    # sample processes
    for worker_id in range(num_sample_worker):
        p = mp.Process(target=run_sample, args=(
            worker_id, run_config))
        p.start()
        workers.append(p)
        # sampler switcher
        p = mp.Process(target=run_train, args=(
            worker_id, run_config, TrainerType.Switcher))
        p.start()
        workers.append(p)

    # train processes
    for worker_id in range(num_train_worker):
        p = mp.Process(target=run_train, args=(
            worker_id, run_config, TrainerType.Trainer))
        p.start()
        workers.append(p)

    for p in workers:
        p.join()
