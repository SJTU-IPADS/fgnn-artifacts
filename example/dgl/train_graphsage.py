import dgl
import torch
import dgl.nn.pytorch as dglnn
import torch.optim as optim
import torch.nn as nn
import torch.nn.functional as F
import fastgraph
import time
import numpy as np


class SAGE(nn.Module):
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
        self.layers.append(dglnn.SAGEConv(in_feats, n_hidden, 'mean'))
        for i in range(1, n_layers - 1):
            self.layers.append(dglnn.SAGEConv(n_hidden, n_hidden, 'mean'))
        self.layers.append(dglnn.SAGEConv(n_hidden, n_classes, 'mean'))
        self.dropout = nn.Dropout(dropout)
        self.activation = activation

    def forward(self, blocks, x):
        h = x
        for l, (layer, block) in enumerate(zip(self.layers, blocks)):
            h = layer(block, h)
            if l != len(self.layers) - 1:
                h = self.activation(h)
                h = self.dropout(h)
        return h


def get_run_config():
    run_config = {}
    run_config['pipeline'] = False
    run_config['device'] = 'cuda:0'
    run_config['dataset'] = 'Papers100M'
    # run_config['dataset'] = 'Com-Friendster'

    if run_config['dataset'] == 'Papers100M':
        run_config['dataset_path'] = '/graph-learning/samgraph/papers100M'
    elif run_config['dataset'] == 'Com-Friendster':
        run_config['dataset_path'] = '/graph-learning/samgraph/com-friendster'
    else:
        assert(False)

    run_config['fanout'] = [15, 10, 5]
    run_config['num_fanout'] = run_config['num_layer'] = len(
        run_config['fanout'])
    run_config['num_epoch'] = 20
    run_config['num_hidden'] = 256
    run_config['batch_size'] = 8192
    run_config['lr'] = 0.003
    run_config['dropout'] = 0.5
    run_config['report_per_count'] = 1

    return run_config


def run():
    run_config = get_run_config()
    device = torch.device(run_config['device'])

    dataset = fastgraph.dataset(
        run_config['dataset'], run_config['dataset_path'])
    g = dataset.to_dgl_graph()
    train_nids = dataset.train_set
    in_feats = dataset.feat_dim
    n_classes = dataset.num_class

    sampler = dgl.dataloading.MultiLayerNeighborSampler(run_config['fanout'])
    dataloader = dgl.dataloading.NodeDataLoader(
        g,
        train_nids,
        sampler,
        batch_size=run_config['batch_size'],
        shuffle=True,
        drop_last=True,
        num_workers=0)

    model = SAGE(in_feats, run_config['num_hidden'], n_classes,
                 run_config['num_layer'], F.relu, run_config['dropout'])
    model = model.to(device)
    loss_fcn = nn.CrossEntropyLoss()
    loss_fcn = loss_fcn.to(device)
    optimizer = optim.Adam(model.parameters(), lr=run_config['lr'])
    num_epoch = run_config['num_epoch']

    model.train()

    for epoch in range(num_epoch):
        sample_times = []
        copy_times = []
        train_times = []
        total_times = []
        num_samples = []

        t0 = time.time()
        for step, (_, _, blocks) in enumerate(dataloader):
            t1 = time.time()
            blocks = [block.int().to(device) for block in blocks]
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

            num_sample = 0
            for block in blocks:
                num_sample += block.num_edges()
            num_samples.append(num_sample)

            print('Epoch {:05d} | Step {:05d} | Samples {:f} | Time {:.4f} | Sample Time {:.4f} | Copy Time {:.4f} | Train time {:4f} |  Loss {:.4f} '.format(
                epoch, step, np.mean(num_samples[1:]), np.mean(total_times[1:]), np.mean(sample_times[1:]), np.mean(copy_times[1:]), np.mean(train_times[1:]), loss))
            t0 = time.time()


if __name__ == '__main__':
    run()
