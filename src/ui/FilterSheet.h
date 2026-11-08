// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "FilterGrid.h"
#include "InputEvent.h"
#include "MoodFilter.h"

// The mood filters' editor (docs/MOOD-FILTERS.md 4.3, 7.1, 7.4, 7.5): the
// one-screen grid the design picked (docs/img/mood-filters/: grid-empty,
// grid-two, grid-set, grid-press, grid-level, grid-zero, grid-dj), opened
// from the Albums list's first row (the Library's violet, counting albums)
// or, from S5, Auto DJ's page (the Output tab's blue, counting tracks):
//
//   36-65    [Clear]   312 of 1,741 albums                       [✕]
//   68-103   Happy | Neutral | Sad                  a tray: pick one
//   112-147  Relaxed | Neutral | Aggressive
//   156-191  Instrumental | Vocals          [Standard •••]  the level
//   200-235  (Acoustic) (Party) (Danceable)         chips: pick any
//
// An overlay over the content area, the tab bar still live, not a pushed
// page (7.6): the Ui closes it as it closes a sheet (closeModal()): a tab
// tap, a language, the library update's fence, a dialog or a sheet
// opening. Its cells, their font (Body, or Small for the whole grid),
// widths and hit tests are filtergrid's (FilterGrid.h). Choices apply at
// once: a tap sets the owner's filter, the owner works out what fits (the
// pass), the count follows; the cross closes it, there is no Cancel. A
// second tap on a tray's pick lets it go; the level cycles Strict >
// Standard > Loose and alone filters nothing (its cell dim); Clear empties
// it and stays open.
//
// Drawn in the overlays' panel sprite (overlayPanel()), on its own card
// (a sheet's handle would hit its title row), a press costing its tray,
// chip or pill alone. A toast over its title row stays on top: its pushes
// leave the toast's rows alone, and the Ui sends a Down there nowhere
// while the toast shows, so an unseen Clear can't be hit (4.3). Loop task
// only.
namespace ui {

// Who opened the editor: the filter it edits, and what fits it (7.6).
class FilterOwner {
public:
  // The filter (moodfilter::Filter::pack()).
  virtual uint16_t filterWord() = 0;
  // A tap's new filter, applied at once (the pass). False: it couldn't be
  // (no memory for the Albums list's array: the owner said so, 4.2), and
  // the old one stays.
  virtual bool setFilterWord(uint16_t q) = 0;
  // The count: what fits the filter now, of how many, albums or tracks.
  virtual uint32_t filterFits() = 0;
  virtual uint32_t filterTotal() = 0;
  virtual filtergrid::Scope filterScope() = 0;
  // The editor closed; `changed`: the filter isn't the one it opened
  // with. Told only when the Ui closes it with notify (closeModal(true):
  // the cross, a tab tap, the fence...), not by suspend() (4.3).
  virtual void onFiltersClosed(bool changed) = 0;

protected:
  ~FilterOwner() = default;
};

class FilterSheet {
public:
  static constexpr int kY = filtergrid::kPanelY;  // 36, to the screen's bottom

  // `accent`: the picks and the live level dot (the owner's section's).
  void open(FilterOwner* owner, uint16_t accent);
  void close() { up_ = false; }
  bool up() const { return up_; }
  FilterOwner* owner() const { return up_ ? owner_ : nullptr; }
  uint16_t word() const { return filter_.pack(); }
  // The filter isn't the one it opened with.
  bool changed() const { return filter_.pack() != opened_; }
  // The whole panel.
  void draw();
  // The rows a toast covered (y 36-71: the title row, row 1's top), as
  // the panel holds them, when it goes (Ui::uncover()).
  void drawTop();
  // The owner's filter changed elsewhere (the console's uiMa): read
  // again, and the panel drawn.
  void sync();
  // A glass event. A Down draws its cell pressed, alone; a Tap returns the
  // cell it acts on: Cross (the Ui closes it), or one for tap() after the
  // Ui's tick; None for nothing (the count, Clear with nothing to clear).
  filtergrid::Cell onEvent(const InputEvent& e);
  // A tap on `c`, in 7.5's order: the new state, its tray, chip or pill
  // pushed (Clear: the whole grid), the pass (the owner's
  // setFilterWord()), the count pushed. The finger's answer never waits on
  // the pass. The level cell and Clear are pushed only when what they show
  // flips (a filter on or off; something to clear or not).
  void tap(filtergrid::Cell c);

private:
  // The panel as it is (the panel's whole picture); a unit of it.
  void render();
  void renderClear();
  void renderCount();
  void renderCross();
  void renderTray(int row);
  void renderLevel();
  void renderChip(filtergrid::Cell c);
  // `c`'s unit (its tray, the level cell, its chip, a pill) rendered and
  // pushed alone, over the panel's picture (rendered whole first if
  // another overlay's is there).
  void drawUnit(filtergrid::Cell c);
  void drawCount();
  // The filter is `f` after a tap on `c`: drawn (above).
  void show(filtergrid::Cell c, const moodfilter::Filter& f);
  // A screen rectangle of the panel pushed (over the page, under a toast).
  void push(int x, int y, int w, int h);

  FilterOwner* owner_ = nullptr;
  moodfilter::Filter filter_;
  uint16_t opened_ = 0;  // the filter it opened with
  filtergrid::Words words_;
  filtergrid::Layout layout_;
  filtergrid::Cell pressed_ = filtergrid::Cell::None;
  uint16_t accent_ = 0;
  bool up_ = false;
};

}  // namespace ui
