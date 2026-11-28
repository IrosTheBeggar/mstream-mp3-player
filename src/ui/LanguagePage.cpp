// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Output > Language (the page's shape is in Pages.h): the languages, each
// in its own name; a tap uses one at once. A fresh unit's first screen too.
#include "UiStrings.h"
#include "UiText.h"
#include "ui/Fonts.h"
#include "ui/OutputParts.h"
#include "ui/Pages.h"
#include "ui/Theme.h"
#include "ui/Ui.h"

namespace ui {

using uistr::S;
using namespace outputparts;

void LanguagePage::enter(NavModel::PageRef& ref) {
  firstBoot_ = ref.id == kFirstBootPicker;
  // The languages open at the one in use (with the one before it above).
  const int lang = static_cast<int>(uistr::lang());
  show(ref, lang > 0 ? (lang - 1) * ListLayout::kPitch : 0);
}

Header LanguagePage::header() const {
  Header h;
  h.subWhole = true;  // (a phrase: whole beside the title, or not at all)
  // The first boot's: nothing behind it, and where it is found later.
  h.back = !firstBoot_;
  h.title = tr(S::language_title);
  h.sub = tr(firstBoot_ ? S::language_first_boot_sub : S::language_subtitle);
  return h;
}

uint32_t LanguagePage::rows() { return uistr::kLangs; }

// A language on its page: its name in itself, Bold with the radio on for
// the one in use (UiText's kLangRowX and kLangRowW, measured by
// test_ui_strings).
void LanguagePage::drawRow(ListView::Row& r) {
  if (r.row >= uistr::kLangs) return;
  const auto l = static_cast<uistr::Lang>(r.row);
  const bool on = l == uistr::lang();
  Fonts::instance().draw(r.c, on ? Font::Bold : Font::Body, uistr::autonym(l), r.x + uitext::kLangRowX, 21,
                         uitext::kLangRowW, col::TXT, r.bg);
  radio(r.c, r.right - 22, 21, on, accent::Output, r.bg);
}

ListView::Tap LanguagePage::onTapAt(uint32_t row, int x, bool rightEdge) {
  (void)x;
  (void)rightEdge;
  // At once: saved, and everything drawn again in it (the first boot's
  // goes on to Now Playing).
  if (row < uistr::kLangs) ui_.chooseLanguage(static_cast<uistr::Lang>(row));
  return ListView::Tap::Handled;
}

}  // namespace ui
