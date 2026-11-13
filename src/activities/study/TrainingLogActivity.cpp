#include "TrainingLogActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* STATUSES[] = {"Done", "Modified", "Missed"};
constexpr int STATUS_COUNT = 3;
constexpr const char* LABELS[] = {"STATUS", "DISTANCE", "PACE", "AVG HR", "RPE", "NOTES", ""};
constexpr int ROW = 84;
constexpr int BTN_W = 56;
constexpr int BTN_H = 48;
constexpr int BTN_GAP = 8;

// First "m:ss" in a text like "4:25-4:35/km"; 0 if none.
int firstPace(const std::string& s) {
  for (size_t i = 0; i + 3 < s.size(); ++i) {
    if (isdigit(static_cast<unsigned char>(s[i])) && s[i + 1] == ':' && isdigit(static_cast<unsigned char>(s[i + 2])) &&
        isdigit(static_cast<unsigned char>(s[i + 3]))) {
      size_t start = i;
      if (start > 0 && isdigit(static_cast<unsigned char>(s[start - 1]))) --start;
      return std::atoi(s.substr(start, i - start + 1).c_str()) * 60 + std::atoi(s.substr(i + 2, 2).c_str());
    }
  }
  return 0;
}

std::string mmss(const int sec) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d", sec / 60, sec % 60);
  return buf;
}

// Small and big steps per field; button labels to match.
struct Steps {
  float small, big;
  const char* labels[4];  // big down, small down, small up, big up
};
Steps stepsFor(const int field) {
  switch (field) {
    case 1:
      return {0.1f, 1.0f, {"-1", "-.1", "+.1", "+1"}};
    case 2:
      return {1, 10, {"-10s", "-1s", "+1s", "+10s"}};
    case 3:
      return {1, 10, {"-10", "-1", "+1", "+10"}};
    default:
      return {1, 0, {nullptr, "-", "+", nullptr}};
  }
}
}  // namespace

TrainingLogActivity::TrainingLogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         training::Session session)
    : Activity("TrainingLog", renderer, mappedInput), session(std::move(session)) {}

void TrainingLogActivity::onEnter() {
  Activity::onEnter();
  ActivityResult cancelled;
  cancelled.isCancelled = true;
  setResult(std::move(cancelled));
  const auto& s = session;
  status = s.status == "Modified" ? 1 : s.status == "Missed" ? 2 : 0;
  km = s.actualKm > 0 ? s.actualKm : s.plannedKm;
  paceSec = firstPace(s.actualPace);
  if (paceSec == 0) paceSec = firstPace(s.targetPace);
  if (paceSec == 0) paceSec = 330;
  hr = s.hr > 0 ? s.hr : 150;
  rpe = s.rpe > 0 ? s.rpe : 6;
  notes = s.notes;
  requestUpdate();
}

bool TrainingLogActivity::fieldShown(const int field) const {
  if (field == Km || field == Pace || field == Hr) return session.isRun() && status != 2;
  if (field == Rpe) return status != 2;
  return true;
}

void TrainingLogActivity::change(const int field, const int steps) {
  const Steps st = stepsFor(field);
  {
    RenderLock lock(*this);
    switch (field) {
      case Status:
        status = (status + (steps > 0 ? 1 : STATUS_COUNT - 1)) % STATUS_COUNT;
        break;
      case Km: {
        const float d = (steps == 2 || steps == -2) ? st.big : st.small;
        km = std::max(0.0f, std::min(60.0f, km + (steps > 0 ? d : -d)));
        km = static_cast<float>(static_cast<int>(km * 10 + 0.5f)) / 10;
        break;
      }
      case Pace: {
        const int d = (steps == 2 || steps == -2) ? 10 : 1;
        paceSec = std::max(150, std::min(900, paceSec + (steps > 0 ? d : -d)));
        break;
      }
      case Hr: {
        const int d = (steps == 2 || steps == -2) ? 10 : 1;
        hr = std::max(60, std::min(220, hr + (steps > 0 ? d : -d)));
        break;
      }
      case Rpe:
        rpe = std::max(1, std::min(10, rpe + (steps > 0 ? 1 : -1)));
        break;
      default:
        break;
    }
  }
  requestUpdate();
}

std::string TrainingLogActivity::valueText(const int field) const {
  char buf[32];
  switch (field) {
    case Status:
      return STATUSES[status];
    case Km:
      snprintf(buf, sizeof(buf), "%.1f km", km);
      return buf;
    case Pace:
      return mmss(paceSec) + " /km";
    case Hr:
      return std::to_string(hr) + " bpm";
    case Rpe:
      return std::to_string(rpe) + " / 10";
    case Notes:
      return notes.empty() ? "Tap to add a note" : notes;
    default:
      return "";
  }
}

void TrainingLogActivity::editNotes() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, "Notes", notes, 300, InputType::Text);
  if (!keyboard) {
    LOG_ERR("TRAIN", "OOM: notes keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    swallowBack = true;
    if (!result.isCancelled) notes = std::get<KeyboardResult>(result.data).text;
    requestUpdate();
  });
}

void TrainingLogActivity::save() {
  training::Change c;
  c.pageId = session.id;
  c.status = STATUSES[status];
  session.status = c.status;
  if (status != 2) {
    c.rpe = rpe;
    session.rpe = rpe;
    if (session.isRun()) {
      c.actualKm = km;
      c.actualPace = mmss(paceSec);
      c.hr = hr;
      session.actualKm = km;
      session.actualPace = c.actualPace;
      session.hr = hr;
    }
  }
  c.notes = notes;
  session.notes = notes;
  if (session.id.empty()) {
    // A session made on the device that has not reached Notion yet.
    c.title = session.title;
    c.date = session.date;
    c.type = session.type;
    c.week = training::weekOf(session.date);
  }
  if (!training::queue(c)) {
    LOG_ERR("TRAIN", "Could not queue log");
    return;
  }
  // Show the log right away; the next sync replaces it with Notion's copy.
  std::vector<training::Session> all;
  training::loadSessions(all);
  for (auto& s : all) {
    if (s.id == session.id && s.date == session.date && s.title == session.title) s = session;
  }
  training::saveSessions(all);
  setResult(ActivityResult{});
  finish();
}

void TrainingLogActivity::loop() {
  using B = MappedInputManager::Button;
  // The Back that closed the keyboard must not close this screen too.
  if (swallowBack) {
    if (mappedInput.wasReleased(B::Back)) {
      swallowBack = false;
      return;
    }
    if (!mappedInput.isPressed(B::Back)) swallowBack = false;
  }
  if (mappedInput.wasReleased(B::Back)) {
    finish();
    return;
  }
  const auto move = [&](int dir) {
    int f = focus;
    do {
      f = (f + dir + FIELD_COUNT) % FIELD_COUNT;
    } while (!fieldShown(f));
    {
      RenderLock lock(*this);
      focus = f;
    }
    requestUpdate();
  };
  if (mappedInput.wasReleased(B::Down) || mappedInput.wasReleased(B::PageForward)) return move(1);
  if (mappedInput.wasReleased(B::Up) || mappedInput.wasReleased(B::PageBack)) return move(-1);
  if (mappedInput.wasReleased(B::Right)) return change(focus, 1);
  if (mappedInput.wasReleased(B::Left)) return change(focus, -1);
  if (mappedInput.wasReleased(B::Confirm)) {
    if (focus == Save) return save();
    if (focus == Notes) return editNotes();
    return move(1);
  }
  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return;
  for (const auto& h : hits) {
    if (x < h.x0 || x >= h.x1 || y < h.y0 || y >= h.y1) continue;
    if (h.field == Save) return save();
    if (h.field == Notes) return editNotes();
    if (h.steps != 0) return change(h.field, h.steps);
    {
      RenderLock lock(*this);
      focus = h.field;
    }
    return requestUpdate();
  }
}

void TrainingLogActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pad = m.contentSidePadding;
  hits.clear();
  GUI.drawHeader(renderer, Rect{0, m.topPadding, w, m.headerHeight}, "LOG SESSION", nullptr);

  int y = m.topPadding + m.headerHeight + 12;
  const std::string meta = session.type + "  " + session.date;
  renderer.drawText(SMALL_FONT_ID, pad, y, meta.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(SMALL_FONT_ID) + 2;
  for (const auto& l :
       renderer.wrappedText(UI_12_FONT_ID, session.title.c_str(), w - 2 * pad, 2, EpdFontFamily::BOLD)) {
    renderer.drawText(UI_12_FONT_ID, pad, y, l.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID);
  }
  if (!session.workout.empty()) {
    for (const auto& l : renderer.wrappedText(SMALL_FONT_ID, session.workout.c_str(), w - 2 * pad, 2)) {
      renderer.drawText(SMALL_FONT_ID, pad, y, l.c_str());
      y += renderer.getLineHeight(SMALL_FONT_ID);
    }
  }
  y += 12;
  renderer.drawLine(pad, y, w - pad, y);

  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  for (int f = Status; f < Save; ++f) {
    if (!fieldShown(f)) continue;
    const bool sel = focus == f;
    if (sel) renderer.fillRect(pad - 10, y + 10, 4, ROW - 20, true);
    renderer.drawText(SMALL_FONT_ID, pad, y + 12, LABELS[f], true, EpdFontFamily::BOLD);
    const std::string value = valueText(f);
    if (f == Notes) {
      const auto lines = renderer.wrappedText(UI_12_FONT_ID, value.c_str(), w - 2 * pad, 2);
      int ly = y + 12 + smallH + 4;
      for (const auto& l : lines) {
        renderer.drawText(UI_12_FONT_ID, pad, ly, l.c_str(), true,
                          notes.empty() ? EpdFontFamily::ITALIC : EpdFontFamily::REGULAR);
        ly += renderer.getLineHeight(UI_12_FONT_ID);
      }
      hits.push_back({0, y, w, y + ROW, f, 0});
    } else {
      renderer.drawText(NOTOSANS_18_FONT_ID, pad, y + 12 + smallH, value.c_str(), true, EpdFontFamily::BOLD);
      const Steps st = stepsFor(f);
      int bx = w - pad;
      const int by = y + (ROW - BTN_H) / 2;
      for (int i = 3; i >= 0; --i) {
        if (f == Status && (i == 0 || i == 3)) continue;
        const char* label = f == Status ? (i == 1 ? "<" : ">") : st.labels[i];
        if (!label) continue;
        bx -= BTN_W;
        renderer.drawRoundedRect(bx, by, BTN_W, BTN_H, 1, 8, true);
        const int tw = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
        renderer.drawText(UI_10_FONT_ID, bx + (BTN_W - tw) / 2,
                          by + (BTN_H - renderer.getLineHeight(UI_10_FONT_ID)) / 2, label, true, EpdFontFamily::BOLD);
        const int steps = i == 0 ? -2 : i == 1 ? -1 : i == 2 ? 1 : 2;
        hits.push_back({bx, by, bx + BTN_W, by + BTN_H, f, steps});
        bx -= BTN_GAP;
      }
      hits.push_back({0, y, bx, y + ROW, f, 0});
    }
    y += ROW;
    renderer.drawLine(pad, y, w - pad, y);
  }

  // Save button.
  y += 20;
  const int bh = 56;
  if (focus == Save) {
    renderer.fillRoundedRect(pad, y, w - 2 * pad, bh, 10, Color::Black);
  } else {
    renderer.drawRoundedRect(pad, y, w - 2 * pad, bh, 2, 10, true);
  }
  static constexpr const char* SAVE = "Save to Notion";
  const int tw = renderer.getTextWidth(UI_12_FONT_ID, SAVE, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, (w - tw) / 2, y + (bh - renderer.getLineHeight(UI_12_FONT_ID)) / 2, SAVE,
                    focus != Save, EpdFontFamily::BOLD);
  hits.push_back({pad, y, w - pad, y + bh, Save, 0});

  const auto labels = mappedInput.mapLabels("Back", focus == Save ? "Save" : "Next", "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
