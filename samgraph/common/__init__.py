import ctypes
import os
import sysconfig


def get_ext_suffix():
    """Determine library extension for various versions of Python."""
    ext_suffix = sysconfig.get_config_var('EXT_SUFFIX')
    if ext_suffix:
        return ext_suffix

    ext_suffix = sysconfig.get_config_var('SO')
    if ext_suffix:
        return ext_suffix

    return '.so'


def get_extension_full_path(pkg_path, *args):
    assert len(args) >= 1
    dir_path = os.path.join(os.path.dirname(pkg_path), *args[:-1])
    full_path = os.path.join(dir_path, args[-1] + get_ext_suffix())
    return full_path


class Context(object):
    def __init__(self, device_type, device_id):
        self.device_type = device_type
        self.device_id = device_id


kCPU = 0
kMMAP = 1
kGPU = 2


def cpu(device_id=0):
    return Context(kCPU, device_id)


def gpu(device_id=0):
    return Context(kGPU, device_id)


kKHop0 = 0
kKHop1 = 1
kWeightedKHop = 2
kRandomWalk = 3

kArch0 = 0
kArch1 = 1
kArch2 = 2
kArch3 = 3

kCacheByDegree = 0
kCacheByHeuristic = 1

meepo_archs = {
    'arch0': {
        'arch_type': kArch0,
        'sampler_ctx': cpu(),
        'trainer_ctx': gpu(0)
    },
    'arch1': {
        'arch_type': kArch1,
        'sampler_ctx': gpu(0),
        'trainer_ctx': gpu(0)
    },
    'arch2': {
        'arch_type': kArch2,
        'sampler_ctx': gpu(0),
        'trainer_ctx': gpu(0)
    },
    'arch3': {
        'arch_type': kArch3,
        'sampler_ctx': gpu(0),
        'trainer_ctx': gpu(1)
    }
}


class SamGraphBasics(object):
    def __init__(self, pkg_path, *args):
        full_path = get_extension_full_path(pkg_path, *args)
        self.C_LIB_CTYPES = ctypes.CDLL(full_path, mode=ctypes.RTLD_GLOBAL)

        self.C_LIB_CTYPES.samgraph_get_next_batch.argtypes = (
            ctypes.c_uint64,
            ctypes.c_uint64)
        self.C_LIB_CTYPES.samgraph_get_graph_num_src.argtypes = (
            ctypes.c_uint64,)
        self.C_LIB_CTYPES.samgraph_get_graph_num_dst.argtypes = (
            ctypes.c_uint64,)
        self.C_LIB_CTYPES.samgraph_get_graph_num_edge.argtypes = (
            ctypes.c_uint64,)
        self.C_LIB_CTYPES.samgraph_report_step.argtypes = (
            ctypes.c_uint64,
            ctypes.c_uint64)
        self.C_LIB_CTYPES.samgraph_report_step_average.argtypes = (
            ctypes.c_uint64,
            ctypes.c_uint64)
        self.C_LIB_CTYPES.samgraph_report_epoch.argtypes = (
            ctypes.c_uint64,)
        self.C_LIB_CTYPES.samgraph_report_epoch_average.argtypes = (
            ctypes.c_uint64,)

        self.C_LIB_CTYPES.samgraph_steps_per_epoch.restype = ctypes.c_size_t
        self.C_LIB_CTYPES.samgraph_num_class.restype = ctypes.c_size_t
        self.C_LIB_CTYPES.samgraph_feat_dim.restype = ctypes.c_size_t
        self.C_LIB_CTYPES.samgraph_get_next_batch.restype = ctypes.c_uint64
        self.C_LIB_CTYPES.samgraph_num_epoch.restype = ctypes.c_size_t
        self.C_LIB_CTYPES.samgraph_get_graph_num_src.restype = ctypes.c_size_t
        self.C_LIB_CTYPES.samgraph_get_graph_num_dst.restype = ctypes.c_size_t
        self.C_LIB_CTYPES.samgraph_get_graph_num_edge.restype = ctypes.c_size_t

    def config(self, run_config):
        return self.C_LIB_CTYPES.samgraph_config(
            ctypes.c_char_p(
                str.encode(run_config['dataset_path'])
            ),
            ctypes.c_int(
                run_config['arch_type']
            ),
            ctypes.c_int(
                run_config['sample_type']
            ),
            ctypes.c_int(
                run_config['sampler_ctx'].device_type
            ),
            ctypes.c_int(
                run_config['sampler_ctx'].device_id
            ),
            ctypes.c_int(
                run_config['trainer_ctx'].device_type
            ),
            ctypes.c_int(
                run_config['trainer_ctx'].device_id
            ),
            ctypes.c_size_t(
                run_config['batch_size']
            ),
            (ctypes.c_int * run_config['num_fanout'])(
                *run_config['fanout']
            ),
            ctypes.c_size_t(
                run_config['num_fanout']
            ),
            ctypes.c_size_t(
                run_config['num_epoch']
            ),
            ctypes.c_int(
                run_config['cache_policy']
            ),
            ctypes.c_double(
                run_config['cache_percentage']
            )
        )

    def init(self):
        return self.C_LIB_CTYPES.samgraph_init()

    def start(self):
        return self.C_LIB_CTYPES.samgraph_start()

    def shutdown(self):
        return self.C_LIB_CTYPES.samgraph_shutdown()

    def num_class(self):
        return self.C_LIB_CTYPES.samgraph_num_class()

    def feat_dim(self):
        return self.C_LIB_CTYPES.samgraph_feat_dim()

    def num_epoch(self):
        return self.C_LIB_CTYPES.samgraph_num_epoch()

    def steps_per_epoch(self):
        return self.C_LIB_CTYPES.samgraph_steps_per_epoch()

    def get_next_batch(self, epoch, step):
        batch_key = self.C_LIB_CTYPES.samgraph_get_next_batch(epoch, step)
        return batch_key

    def get_graph_num_src(self, key, graph_id):
        return self.C_LIB_CTYPES.samgraph_get_graph_num_src(key, graph_id)

    def get_graph_num_dst(self, key, graph_id):
        return self.C_LIB_CTYPES.samgraph_get_graph_num_dst(key, graph_id)

    def sample_once(self):
        return self.C_LIB_CTYPES.samgraph_sample_once()

    def report_step(self, epoch, step):
        return self.C_LIB_CTYPES.samgraph_report_step(epoch, step)

    def report_step_average(self, epoch, step):
        return self.C_LIB_CTYPES.samgraph_report_step_average(epoch, step)

    def report_epoch(self, epoch):
        return self.C_LIB_CTYPES.samgraph_report_epoch(epoch)

    def report_epoch_average(self, epoch):
        return self.C_LIB_CTYPES.samgraph_report_epoch_average(epoch)

    def report_node_access(self):
        return self.C_LIB_CTYPES.samgraph_report_node_access()
