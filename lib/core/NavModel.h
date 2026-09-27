#pragma once
#include <cstdint>

// Where the UI is: the tab bar design's navigation (spec §1, §5). Five tabs,
// each with its own stack of pages; the tab bar switches between them and
// every tab keeps its place. Portable, host-tested; no drawing here.
//
//   - Tapping another tab switches to it, its stack as it was left.
//   - Tapping the tab you are on takes it back to its root page (Library to
//     its list, Queue to the current track...), like phone apps do.
//   - A page pushed on a tab has a "back" ("‹" in its header) that pops it.
//   - Each page remembers its scroll position and its expanded row (the
//     inline action bar under a tapped track), so Back and a return to the
//     tab show it where it was.
//
// A page is a small value (PageRef): what kind of page (the app's own
// numbering), which thing it shows (an artist, an album: an index id), and
// its remembered state. The stacks are fixed arrays: no allocation.
class NavModel {
public:
  enum class Tab : uint8_t { NowPlaying, Library, Queue, Dance, Output };
  static constexpr int kTabs = 5;
  // Deeper pushes drop the oldest page above the root (the Library tree is
  // 3-4 levels; folders could go deeper).
  static constexpr int kMaxDepth = 8;
  static constexpr uint32_t kNone = 0xFFFFFFFFu;

  struct PageRef {
    uint8_t kind = 0;         // the app's page kind; 0: none
    uint32_t id = kNone;      // what the page shows (an artist id, an album id...)
    int32_t scrollPx = -1;    // remembered scroll offset; -1: the page's own default
    int32_t expanded = -1;    // remembered expanded row; -1: none
    bool operator==(const PageRef& o) const {
      return kind == o.kind && id == o.id && scrollPx == o.scrollPx && expanded == o.expanded;
    }
  };

  // What a tap on a tab did.
  enum class TabTap : uint8_t {
    Switched,      // another tab is now current, its stack as it was
    PoppedToRoot,  // the current tab went back to its root page
    AtRoot,        // the current tab was at its root already (the page may scroll home)
  };

  // Each tab's root page; the stack becomes that page alone.
  void setRoot(Tab t, const PageRef& root);

  Tab tab() const { return tab_; }
  TabTap tapTab(Tab t);
  // Switches without the tap's "again: to the root" rule.
  void select(Tab t) { tab_ = t; }

  // Pushes a page on the current tab.
  void push(const PageRef& p);
  // Pops the current tab's top page; false at its root.
  bool back();
  // Tab t's stack becomes its root plus `pages` (e.g. Now
  // Playing's "Go to album": the Library shows that album, one Back from
  // its artist). `n` is cut to fit.
  void replaceAboveRoot(Tab t, const PageRef* pages, int n);
  // Back to the root page (its remembered state kept).
  void popToRoot(Tab t);

  // The page on screen, and each tab's.
  PageRef& top() { return stack_[idx(tab_)][depth_[idx(tab_)] - 1]; }
  const PageRef& top() const { return stack_[idx(tab_)][depth_[idx(tab_)] - 1]; }
  const PageRef& top(Tab t) const { return stack_[idx(t)][depth_[idx(t)] - 1]; }
  PageRef& top(Tab t) { return stack_[idx(t)][depth_[idx(t)] - 1]; }
  int depth(Tab t) const { return depth_[idx(t)]; }
  int depth() const { return depth_[idx(tab_)]; }
  const PageRef& at(Tab t, int level) const { return stack_[idx(t)][level]; }

  static const char* name(Tab t);

private:
  static int idx(Tab t) { return static_cast<int>(t); }

  Tab tab_ = Tab::NowPlaying;
  PageRef stack_[kTabs][kMaxDepth];
  uint8_t depth_[kTabs] = {1, 1, 1, 1, 1};
};
