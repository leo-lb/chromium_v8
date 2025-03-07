// Copyright 2009-2010 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/profiler/heap-profiler.h"

#include "src/api/api-inl.h"
#include "src/debug/debug.h"
#include "src/heap/combined-heap.h"
#include "src/heap/heap-inl.h"
#include "src/profiler/allocation-tracker.h"
#include "src/profiler/heap-snapshot-generator-inl.h"
#include "src/profiler/sampling-heap-profiler.h"

namespace v8 {
namespace internal {

HeapProfiler::HeapProfiler(Heap* heap)
    : ids_(new HeapObjectsMap(heap)),
      names_(new StringsStorage()),
      is_tracking_object_moves_(false) {}

HeapProfiler::~HeapProfiler() = default;

void HeapProfiler::DeleteAllSnapshots() {
  snapshots_.clear();
  MaybeClearStringsStorage();
}

void HeapProfiler::MaybeClearStringsStorage() {
  if (snapshots_.empty() && !sampling_heap_profiler_ && !allocation_tracker_) {
    names_.reset(new StringsStorage());
  }
}

void HeapProfiler::RemoveSnapshot(HeapSnapshot* snapshot) {
  snapshots_.erase(
      std::find_if(snapshots_.begin(), snapshots_.end(),
                   [&](const std::unique_ptr<HeapSnapshot>& entry) {
                     return entry.get() == snapshot;
                   }));
}

void HeapProfiler::AddBuildEmbedderGraphCallback(
    v8::HeapProfiler::BuildEmbedderGraphCallback callback, void* data) {
  build_embedder_graph_callbacks_.push_back({callback, data});
}

void HeapProfiler::RemoveBuildEmbedderGraphCallback(
    v8::HeapProfiler::BuildEmbedderGraphCallback callback, void* data) {
  auto it = std::find(build_embedder_graph_callbacks_.begin(),
                      build_embedder_graph_callbacks_.end(),
                      std::make_pair(callback, data));
  if (it != build_embedder_graph_callbacks_.end())
    build_embedder_graph_callbacks_.erase(it);
}

void HeapProfiler::BuildEmbedderGraph(Isolate* isolate,
                                      v8::EmbedderGraph* graph) {
  for (const auto& cb : build_embedder_graph_callbacks_) {
    cb.first(reinterpret_cast<v8::Isolate*>(isolate), graph, cb.second);
  }
}

HeapSnapshot* HeapProfiler::TakeSnapshot(
    v8::ActivityControl* control,
    v8::HeapProfiler::ObjectNameResolver* resolver) {
  HeapSnapshot* result = new HeapSnapshot(this);
  {
    HeapSnapshotGenerator generator(result, control, resolver, heap());
    if (!generator.GenerateSnapshot()) {
      delete result;
      result = nullptr;
    } else {
      snapshots_.emplace_back(result);
    }
  }
  ids_->RemoveDeadEntries();
  is_tracking_object_moves_ = true;

  heap()->isolate()->debug()->feature_tracker()->Track(
      DebugFeatureTracker::kHeapSnapshot);

  return result;
}

bool HeapProfiler::StartSamplingHeapProfiler(
    uint64_t sample_interval, int stack_depth,
    v8::HeapProfiler::SamplingFlags flags) {
  if (sampling_heap_profiler_.get()) {
    return false;
  }
  sampling_heap_profiler_.reset(new SamplingHeapProfiler(
      heap(), names_.get(), sample_interval, stack_depth, flags));
  return true;
}


void HeapProfiler::StopSamplingHeapProfiler() {
  sampling_heap_profiler_.reset();
  MaybeClearStringsStorage();
}


v8::AllocationProfile* HeapProfiler::GetAllocationProfile() {
  if (sampling_heap_profiler_.get()) {
    return sampling_heap_profiler_->GetAllocationProfile();
  } else {
    return nullptr;
  }
}


void HeapProfiler::StartHeapObjectsTracking(bool track_allocations) {
  ids_->UpdateHeapObjectsMap();
  is_tracking_object_moves_ = true;
  DCHECK(!allocation_tracker_);
  if (track_allocations) {
    allocation_tracker_.reset(new AllocationTracker(ids_.get(), names_.get()));
    heap()->AddHeapObjectAllocationTracker(this);
    heap()->isolate()->debug()->feature_tracker()->Track(
        DebugFeatureTracker::kAllocationTracking);
  }
}

SnapshotObjectId HeapProfiler::PushHeapObjectsStats(OutputStream* stream,
                                                    int64_t* timestamp_us) {
  return ids_->PushHeapObjectsStats(stream, timestamp_us);
}

void HeapProfiler::StopHeapObjectsTracking() {
  ids_->StopHeapObjectsTracking();
  if (allocation_tracker_) {
    allocation_tracker_.reset();
    MaybeClearStringsStorage();
    heap()->RemoveHeapObjectAllocationTracker(this);
  }
}

int HeapProfiler::GetSnapshotsCount() {
  return static_cast<int>(snapshots_.size());
}

HeapSnapshot* HeapProfiler::GetSnapshot(int index) {
  return snapshots_.at(index).get();
}

SnapshotObjectId HeapProfiler::GetSnapshotObjectId(Handle<Object> obj) {
  if (!obj->IsHeapObject())
    return v8::HeapProfiler::kUnknownObjectId;
  return ids_->FindEntry(HeapObject::cast(*obj).address());
}

void HeapProfiler::ObjectMoveEvent(Address from, Address to, int size) {
  base::MutexGuard guard(&profiler_mutex_);
  bool known_object = ids_->MoveObject(from, to, size);
  if (!known_object && allocation_tracker_) {
    allocation_tracker_->address_to_trace()->MoveObject(from, to, size);
  }
}

void HeapProfiler::AllocationEvent(Address addr, int size) {
  DisallowHeapAllocation no_allocation;
  if (allocation_tracker_) {
    allocation_tracker_->AllocationEvent(addr, size);
  }
}


void HeapProfiler::UpdateObjectSizeEvent(Address addr, int size) {
  ids_->UpdateObjectSize(addr, size);
}

Handle<HeapObject> HeapProfiler::FindHeapObjectById(SnapshotObjectId id) {
  HeapObject object;
  CombinedHeapIterator iterator(heap(), HeapIterator::kFilterUnreachable);
  // Make sure that object with the given id is still reachable.
  for (HeapObject obj = iterator.Next(); !obj.is_null();
       obj = iterator.Next()) {
    if (ids_->FindEntry(obj.address()) == id) {
      DCHECK(object.is_null());
      object = obj;
      // Can't break -- kFilterUnreachable requires full heap traversal.
    }
  }

  return !object.is_null() ? Handle<HeapObject>(object, isolate())
                           : Handle<HeapObject>();
}


void HeapProfiler::ClearHeapObjectMap() {
  ids_.reset(new HeapObjectsMap(heap()));
  if (!allocation_tracker_) is_tracking_object_moves_ = false;
}


Heap* HeapProfiler::heap() const { return ids_->heap(); }

Isolate* HeapProfiler::isolate() const { return heap()->isolate(); }

void HeapProfiler::QueryObjects(Handle<Context> context,
                                debug::QueryObjectPredicate* predicate,
                                PersistentValueVector<v8::Object>* objects) {
  {
    CombinedHeapIterator function_heap_iterator(
        heap(), HeapIterator::kFilterUnreachable);
    for (HeapObject heap_obj = function_heap_iterator.Next();
         !heap_obj.is_null(); heap_obj = function_heap_iterator.Next()) {
      if (heap_obj.IsFeedbackVector()) {
        FeedbackVector::cast(heap_obj).ClearSlots(isolate());
      }
    }
  }
  // We should return accurate information about live objects, so we need to
  // collect all garbage first.
  heap()->CollectAllAvailableGarbage(GarbageCollectionReason::kHeapProfiler);
  CombinedHeapIterator heap_iterator(heap(), HeapIterator::kFilterUnreachable);
  for (HeapObject heap_obj = heap_iterator.Next(); !heap_obj.is_null();
       heap_obj = heap_iterator.Next()) {
    if (!heap_obj.IsJSObject() || heap_obj.IsExternal(isolate())) continue;
    v8::Local<v8::Object> v8_obj(
        Utils::ToLocal(handle(JSObject::cast(heap_obj), isolate())));
    if (!predicate->Filter(v8_obj)) continue;
    objects->Append(v8_obj);
  }
}

}  // namespace internal
}  // namespace v8
