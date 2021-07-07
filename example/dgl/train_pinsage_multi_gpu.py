import argparse
import dgl
import torch
import torch.optim as optim
import torch.nn as nn
import torch.nn.functional as F
import dgl.multiprocessing as mp
from torch.nn.parallel import DistributedDataParallel
import dgl.function as fn
import fastgraph
import time
import numpy as np

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


class PinSAGESampler(object):
    def __init__(self, g, random_walk_length, random_walk_restart_prob, num_random_walk, num_neighbor, num_layer):
        self.g = g
        self.num_layer = num_layer
        self.sampler = dgl.sampling.RandomWalkNeighborSampler(
            g, random_walk_length, random_walk_restart_prob, num_random_walk, num_neighbor)

    def sample_blocks(self, _, seed_nodes):
        blocks = []
        for _ in range(self.num_layer):
            frontier = self.sampler(seed_nodes)
            block = dgl.to_block(frontier, seed_nodes)
            seed_nodes = block.srcdata[dgl.NID]
            blocks.insert(0, block)
        return blocks

    # In samgraph, we call this procedure feature extration.
    def assign_features_to_blocks(self, blocks):
        src_ids = blocks[0].srcdata[dgl.NID].to(torch.long)
        dst_ids = blocks[-1].dstdata[dgl.NID].to(torch.long)
        blocks[0].srcdata['feat'] = self.g.ndata['feat'][src_ids]
        blocks[-1].dstdata['label'] = self.g.ndata['label'][dst_ids]


def parse_args():
    argparser = argparse.ArgumentParser("PinSAGE Training")
    argparser.add_argument('--parse-args', action='store_true', default=False)
    argparser.add_argument('--devices', nargs='+',
                           type=int, default=[0, 1])
    argparser.add_argument('--dataset', type=str,
                           default='com-friendster')
    argparser.add_argument('--root-path', type=str,
                           default='/graph-learning/samgraph/')
    argparser.add_argument('--pipelining', action='store_true', default=False)

    argparser.add_argument('--random-walk-length', type=int, default=3)
    argparser.add_argument('--random-walk-restart-prob',
                           type=float, default=0.5)
    argparser.add_argument('--num-random-walk', type=int, default=4)
    argparser.add_argument('--num-neighbor', type=int, default=5)
    argparser.add_argument('--num-layer', type=int, default=3)
    argparser.add_argument('--num-epoch', type=int, default=11)
    argparser.add_argument('--num-hidden', type=int, default=256)
    argparser.add_argument('--batch-size', type=int, default=8000)
    argparser.add_argument('--lr', type=float, default=0.003)
    argparser.add_argument('--dropout', type=float, default=0.5)

    run_config = vars(argparser.parse_args())
    run_config['num_worker'] = len(run_config['devices'])

    if run_config['pipelining'] == True:
        run_config['num_sampling_worker'] = 16 // run_config['num_worker']
    else:
        run_config['num_sampling_worker'] = 0

    return run_config


def get_run_config():
    args_run_config = parse_args()
    if args_run_config['parse_args']:
        return args_run_config

    run_config = {}
    run_config['devices'] = [0, 1]
    run_config['num_worker'] = len(run_config['devices'])
    # run_config['dataset'] = 'reddit'
    run_config['dataset'] = 'products'
    # run_config['dataset'] = 'papers100M'
    # run_config['dataset'] = 'com-friendster'
    run_config['root_path'] = '/graph-learning/samgraph/'
    run_config['pipelining'] = False

    run_config['random_walk_length'] = 4
    run_config['random_walk_restart_prob'] = 0.5
    run_config['num_random_walk'] = 4
    run_config['num_neighbor'] = 8
    run_config['num_layer'] = 3
    # we use the average result of 10 epochs, the first epoch is used to warm up the system
    run_config['num_epoch'] = 11
    run_config['num_hidden'] = 256
    run_config['batch_size'] = 8000
    run_config['lr'] = 0.003
    run_config['dropout'] = 0.5

    if run_config['pipelining'] == True:
        run_config['num_sampling_worker'] = 16
    else:
        run_config['num_sampling_worker'] = 0

    return run_config


def run(worker_id, run_config):
    dev_id = run_config['devices'][worker_id]
    num_worker = run_config['num_worker']

    if num_worker > 1:
        dist_init_method = 'tcp://{master_ip}:{master_port}'.format(
            master_ip='127.0.0.1', master_port='12345')
        world_size = num_worker
        torch.distributed.init_process_group(backend="nccl",
                                             init_method=dist_init_method,
                                             world_size=world_size,
                                             rank=worker_id)
    torch.cuda.set_device(dev_id)

    dataset = run_config['dataset']
    g = run_config['g']

    train_nids = dataset.train_set
    in_feats = dataset.feat_dim
    n_classes = dataset.num_class

    sampler = PinSAGESampler(g, run_config['random_walk_length'], run_config['random_walk_restart_prob'],
                             run_config['num_random_walk'], run_config['num_neighbor'], run_config['num_layer'])
    dataloader = dgl.dataloading.NodeDataLoader(
        g,
        train_nids,
        sampler,
        use_ddp=num_worker > 1,
        batch_size=run_config['batch_size'],
        shuffle=True,
        drop_last=False,
        num_workers=run_config['num_sampling_worker'])

    model = PinSAGE(in_feats, run_config['num_hidden'], n_classes,
                    run_config['num_layer'], F.relu, run_config['dropout'])
    model = model.to(dev_id)
    if num_worker > 1:
        model = DistributedDataParallel(
            model, device_ids=[dev_id], output_device=dev_id)

    loss_fcn = nn.CrossEntropyLoss()
    loss_fcn = loss_fcn.to(dev_id)
    optimizer = optim.Adam(model.parameters(), lr=run_config['lr'])
    num_epoch = run_config['num_epoch']

    model.train()

    epoch_sample_times = []
    epoch_copy_times = []
    epoch_train_times = []
    epoch_total_times = []

    sample_times = []
    copy_times = []
    train_times = []
    total_times = []
    num_nodes = []
    num_samples = []

    for epoch in range(num_epoch):
        epoch_sample_time = 0.0
        epoch_copy_time = 0.0
        epoch_train_time = 0.0
        epoch_total_time = 0.0

        t0 = time.time()
        for step, (_, _, blocks) in enumerate(dataloader):
            t1 = time.time()
            sampler.assign_features_to_blocks(blocks)
            blocks = [block.int().to(dev_id) for block in blocks]
            batch_inputs = blocks[0].srcdata['feat']
            batch_labels = blocks[-1].dstdata['label']
            t2 = time.time()

            # Compute loss and prediction
            batch_pred = model(blocks, batch_inputs)
            loss = loss_fcn(batch_pred, batch_labels)
            optimizer.zero_grad()
            loss.backward()
            optimizer.step()

            t3 = time.time()

            sample_times.append(t1 - t0)
            copy_times.append(t2 - t1)
            train_times.append(t3 - t2)
            total_times.append(t3 - t0)

            epoch_sample_time += (t1 - t0)
            epoch_copy_time += (t2 - t1)
            epoch_train_time += (t3 - t2)
            epoch_total_time += (t3 - t0)

            num_sample = 0
            for block in blocks:
                num_sample += block.num_edges()
            num_samples.append(num_sample)
            num_nodes.append(blocks[0].num_src_nodes())

            if worker_id == 0:
                print('Epoch {:05d} | Step {:05d} | Nodes {:.0f} | Samples {:.0f} | Time {:.4f} | Sample Time {:.4f} | Copy Time {:.4f} | Train time {:4f} |  Loss {:.4f} '.format(
                    epoch, step, np.mean(num_nodes), np.mean(num_samples), np.mean(total_times[1:]), np.mean(sample_times[1:]), np.mean(copy_times[1:]), np.mean(train_times[1:]), loss))
            t0 = time.time()

        if num_worker > 1:
            torch.distributed.barrier()

        epoch_sample_times.append(epoch_sample_time)
        epoch_copy_times.append(epoch_copy_time)
        epoch_train_times.append(epoch_train_time)
        epoch_total_times.append(epoch_total_time)

    if num_worker > 1:
        torch.distributed.barrier()

    if worker_id == 0:
        print('Avg Epoch Time {:.4f} | Sample Time {:.4f} | Copy Time {:.4f} | Train Time {:.4f}'.format(
            np.mean(epoch_total_times[1:]), np.mean(epoch_sample_times[1:]), np.mean(epoch_copy_times[1:]), np.mean(epoch_train_times[1:])))


if __name__ == '__main__':
    run_config = get_run_config()
    print(run_config)

    dataset = fastgraph.dataset(
        run_config['dataset'], run_config['root_path'])
    # the DGL randomwalk implementation needs a csr graph, but actually the graph is in csc format
    # we pretend to load the graph in csr format so that DGL won't need to convert the graph format.
    g = dataset.to_dgl_graph(g_format='csr')
    run_config['dataset'] = dataset
    run_config['g'] = g

    num_worker = run_config['num_worker']

    if num_worker == 1:
        run(run_config)
    else:
        workers = []
        for worker_id in range(num_worker):
            p = mp.Process(target=run, args=(worker_id, run_config))
            p.start()
            workers.append(p)
        for p in workers:
            p.join()
