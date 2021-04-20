#include "workspace_pool.h"

#include <memory>

#include "device.h"
#include "logging.h"

namespace samgraph {
namespace common {

// page size.
constexpr size_t kWorkspacePageSize = 4 << 10;

class WorkspacePool::Pool {
 public:
  Pool() {
    // List gurad
    Entry e;
    e.data = nullptr;
    e.size = 0;

    _free_list.reserve(kListSize);
    _allocated.reserve(kListSize);

    _free_list.push_back(e);
    _allocated.push_back(e);
  }

  // allocate from pool
  void *Alloc(Context ctx, Device *device, size_t nbytes, size_t scale_factor) {
    // Allocate align to page.
    nbytes = (nbytes + (kWorkspacePageSize - 1)) / kWorkspacePageSize *
             kWorkspacePageSize;
    if (nbytes == 0) nbytes = kWorkspacePageSize;
    nbytes *= scale_factor;

    Entry e;
    if (_free_list.size() == 1) {
      e.data = device->AllocDataSpace(ctx, nbytes, kTempAllocaAlignment);
      e.size = nbytes;
    } else {
      if (_free_list.back().size >= nbytes) {
        // find smallest fit
        auto it = _free_list.end() - 2;
        for (; it->size >= nbytes; --it) {
        }
        e = *(it + 1);
        _free_list.erase(it + 1);
      } else {
        e.data = device->AllocDataSpace(ctx, nbytes, kTempAllocaAlignment);
        e.size = nbytes;
      }
    }
    _allocated.push_back(e);
    return e.data;
  }

  // free resource back to pool
  void Free(void *data) {
    Entry e;
    if (_allocated.back().data == data) {
      // quick path, last allocated.
      e = _allocated.back();
      _allocated.pop_back();
    } else {
      int index = static_cast<int>(_allocated.size()) - 2;
      for (; index > 0 && _allocated[index].data != data; --index) {
      }
      CHECK_GT(index, 0) << "trying to free things that has not been allocated";
      e = _allocated[index];
      _allocated.erase(_allocated.begin() + index);
    }

    if (_free_list.back().size < e.size) {
      _free_list.push_back(e);
    } else {
      size_t i = _free_list.size() - 1;
      _free_list.resize(_free_list.size() + 1);
      for (; e.size < _free_list[i].size; --i) {
        _free_list[i + 1] = _free_list[i];
      }
      _free_list[i + 1] = e;
    }
  }

  // Release all resources
  void Release(Context ctx, Device *device) {
    CHECK_EQ(_allocated.size(), 1);
    for (size_t i = 1; i < _free_list.size(); ++i) {
      device->FreeDataSpace(ctx, _free_list[i].data);
    }
    _free_list.clear();
  }

 private:
  /*! \brief a single entry in the pool */
  struct Entry {
    void *data;
    size_t size;
  };

  std::vector<Entry> _free_list;
  std::vector<Entry> _allocated;

  constexpr static size_t kListSize = 100;
};

WorkspacePool::WorkspacePool(DeviceType device_type,
                             std::shared_ptr<Device> device)
    : _device_type(device_type), _device(device) {}

WorkspacePool::~WorkspacePool() {
  for (size_t i = 0; i < _array.size(); ++i) {
    if (_array[i] != nullptr) {
      Context ctx;
      ctx.device_type = _device_type;
      ctx.device_id = static_cast<int>(i);
      _array[i]->Release(ctx, _device.get());
      delete _array[i];
    }
  }
}

void *WorkspacePool::AllocWorkspace(Context ctx, size_t size,
                                    size_t scale_factor) {
  if (static_cast<size_t>(ctx.device_id) >= _array.size()) {
    _array.resize(ctx.device_id + 1, nullptr);
  }
  if (_array[ctx.device_id] == nullptr) {
    _array[ctx.device_id] = new Pool();
  }
  return _array[ctx.device_id]->Alloc(ctx, _device.get(), size, scale_factor);
}

void WorkspacePool::FreeWorkspace(Context ctx, void *ptr) {
  CHECK(static_cast<size_t>(ctx.device_id) < _array.size() &&
        _array[ctx.device_id] != nullptr);
  _array[ctx.device_id]->Free(ptr);
}

}  // namespace common
}  // namespace samgraph
