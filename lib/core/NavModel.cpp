#include "NavModel.h"

void NavModel::setRoot(Tab t, const PageRef& root) {
  stack_[idx(t)][0] = root;
  depth_[idx(t)] = 1;
}

NavModel::TabTap NavModel::tapTab(Tab t) {
  if (t != tab_) {
    tab_ = t;
    return TabTap::Switched;
  }
  if (depth_[idx(t)] > 1) {
    popToRoot(t);
    return TabTap::PoppedToRoot;
  }
  return TabTap::AtRoot;
}

void NavModel::push(const PageRef& p) {
  const int i = idx(tab_);
  if (depth_[i] == kMaxDepth) {
    // Full: the oldest page above the root goes, the rest move down.
    for (int k = 1; k + 1 < kMaxDepth; ++k) stack_[i][k] = stack_[i][k + 1];
    --depth_[i];
  }
  stack_[i][depth_[i]++] = p;
}

bool NavModel::back() {
  const int i = idx(tab_);
  if (depth_[i] <= 1) return false;
  --depth_[i];
  return true;
}

void NavModel::replaceAboveRoot(Tab t, const PageRef* pages, int n) {
  const int i = idx(t);
  if (n < 0) n = 0;
  if (n > kMaxDepth - 1) n = kMaxDepth - 1;
  for (int k = 0; k < n; ++k) stack_[i][1 + k] = pages[k];
  depth_[i] = static_cast<uint8_t>(1 + n);
}

void NavModel::popToRoot(Tab t) { depth_[idx(t)] = 1; }

const char* NavModel::name(Tab t) {
  switch (t) {
    case Tab::NowPlaying: return "Now Playing";
    case Tab::Library: return "Library";
    case Tab::Queue: return "Queue";
    case Tab::Dance: return "Dance";
    case Tab::Output: return "Output";
  }
  return "?";
}
