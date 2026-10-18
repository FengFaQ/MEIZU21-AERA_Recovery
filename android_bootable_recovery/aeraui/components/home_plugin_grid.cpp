/* SPDX-License-Identifier: Apache-2.0 */
#include "home_plugin_grid.hpp"
#include "ui_components.hpp"
#include "aeraui/home_layout.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace aeraui {
namespace {
using namespace widgets;
struct Tile {
  plugins::Plugin plugin;
  int size = 0;
  lv_obj_t *object = nullptr;
};
struct Grid {
  lv_obj_t *screen = nullptr, *view = nullptr, *dots = nullptr, *toolbar = nullptr;
  lv_obj_t *trash = nullptr, *title = nullptr, *ghost = nullptr;
  lv_obj_t *sizes[kHomePluginSizes.size()] = {};
  int size_rows = 0;
  lv_timer_t *timer = nullptr;
  lv_indev_t *input = nullptr;
  HomePluginGeometry geometry;
  HomePluginTileFactory factory;
  std::function<void(const plugins::Plugin &)> launch;
  ActionCallback callback = nullptr;
  void *context = nullptr;
  std::vector<Tile> tiles;
  std::vector<lv_obj_t *> pages;
  std::vector<HomePluginCell> cells;
  std::vector<HomePluginPreference> before;
  std::string selected, suppress_click;
  lv_point_t point{}, offset{};
  bool editing = false, dragging = false, deleting = false;
  uint32_t edge_since = 0, vertical_since = 0, vertical_step = 0;
  int edge_direction = 0, vertical_direction = 0, vertical_page = -1, page_count = 1;
};
struct Binding { Grid *grid; std::string id; };

int Index(const Grid *state, const std::string &id) {
  for (size_t i = 0; i < state->tiles.size(); ++i)
    if (state->tiles[i].plugin.id == id) return static_cast<int>(i);
  return -1;
}
std::vector<HomePluginPreference> Preferences(const Grid *state) {
  std::vector<HomePluginPreference> result;
  for (const auto &tile : state->tiles) result.push_back({tile.plugin.id, tile.size});
  return result;
}
lv_area_t Bounds(const Grid *state, const HomePluginCell &cell) {
  const auto &g = state->geometry;
  const int gap = g.columns == 2 ? 40 : 32;
  const int width = g.icons ? 170 : (g.width - gap * (g.columns - 1)) / g.columns;
  const int x = g.icons ? cell.column * (g.width - width) / (g.columns - 1)
                        : cell.column * (width + gap);
  const int y = g.icons ? (g.rows == 1 ? (g.height - g.tile_height) / 2 :
                          cell.row * (g.height - g.tile_height) / (g.rows - 1))
                        : cell.row * (g.tile_height + 30);
  const int last_x = g.icons ? (cell.column + cell.width - 1) *
      (g.width - width) / (g.columns - 1) : x + (cell.width - 1) * (width + gap);
  const int last_y = g.icons ? (g.rows == 1 ? y : (cell.row + cell.height - 1) *
      (g.height - g.tile_height) / (g.rows - 1)) : y + (cell.height - 1) * (g.tile_height + 30);
  return {x, y, last_x + width - 1, last_y + g.tile_height - 1};
}
int CurrentPage(const Grid *state) {
  return std::clamp((lv_obj_get_scroll_x(state->view) + state->geometry.width / 2) /
                        state->geometry.width, 0, state->page_count - 1);
}
void UpdateDots(Grid *state) {
  if (!state->dots) return;
  const int selected = CurrentPage(state);
  for (int i = 0; i < state->page_count; ++i) {
    auto *dot = lv_obj_get_child(state->dots, i);
    if (!dot) continue;
    lv_obj_set_size(dot, i == selected ? 34 : 14, 14);
    lv_obj_set_style_bg_color(dot, i == selected ? kAccent : kMainLine, 0);
  }
}
void StopEditing(Grid *state);
void Reflow(Grid *state, bool animate = false);
void Press(lv_event_t *event);
void ArrangeViewport(Grid *state) {
  lv_obj_update_layout(state->screen);
  int height = state->geometry.height;
  if (state->editing) {
    lv_area_t toolbar{}; lv_obj_get_coords(state->toolbar, &toolbar);
    height = std::min(height, std::max(40, toolbar.y1 - state->geometry.y - 40));
  }
  lv_obj_set_height(state->view, height);
  lv_obj_set_pos(state->dots, state->geometry.x,
      state->geometry.y + height + (state->editing ? 8 : 18));
  lv_obj_set_height(state->dots, state->editing ? 24 : 40);
  for (auto *page : state->pages) {
    lv_obj_set_height(page, height);
    if (state->editing && height < state->geometry.height) {
      lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_scroll_dir(page, state->dragging ? LV_DIR_NONE : LV_DIR_VER);
      lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLL_ELASTIC);
      lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLL_MOMENTUM);
      if (state->dragging) lv_obj_stop_scroll_anim(page);
      lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
      lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    } else {
      lv_obj_scroll_to_y(page, 0, LV_ANIM_OFF);
      lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    }
  }
}

void Selection(Grid *state) {
  for (auto &tile : state->tiles) {
    if (state->editing && tile.plugin.id == state->selected)
      lv_obj_add_state(tile.object, LV_STATE_USER_1);
    else lv_obj_remove_state(tile.object, LV_STATE_USER_1);
  }
  const int index = Index(state, state->selected);
  if (index < 0) return;
  lv_label_set_text(state->title, state->tiles[index].plugin.name.c_str());
  for (size_t i = 0; i < kHomePluginSizes.size(); ++i) {
    if (!state->sizes[i]) continue;
    const bool selected = state->tiles[index].size == static_cast<int>(i);
    lv_obj_set_style_border_width(state->sizes[i], selected ? 4 : 1, 0);
    lv_obj_set_style_border_color(state->sizes[i], selected ? kAccent : kMainLine, 0);
  }
}
void CreateTile(Grid *state, size_t index) {
  auto &tile = state->tiles[index];
  const auto cell = state->cells[index];
  const auto rect = Bounds(state, cell);
  const std::string id = tile.plugin.id;
  tile.object = state->factory(state->pages[cell.page], tile.plugin, rect.x1, rect.y1,
      lv_area_get_width(&rect), lv_area_get_height(&rect), state->geometry.icons, [state, id] {
        if (state->dragging || state->suppress_click == id) {
          state->suppress_click.clear();
          return;
        }
        if (state->editing) { state->selected = id; Selection(state); return; }
        const int at = Index(state, id);
        if (at >= 0) state->launch(state->tiles[at].plugin);
      });
  lv_obj_set_style_border_width(tile.object, 3, LV_STATE_USER_1);
  lv_obj_set_style_border_color(tile.object, kAccent, LV_STATE_USER_1);
  lv_obj_set_style_border_opa(tile.object, LV_OPA_COVER, LV_STATE_USER_1);
  lv_obj_add_event_cb(tile.object, Press, LV_EVENT_ALL, new Binding{state, id});
}
void Reflow(Grid *state, bool animate) {
  const int current = CurrentPage(state);
  state->cells = PackHomePlugins(Preferences(state), state->geometry.columns,
                                 state->geometry.rows, state->geometry.icons);
  state->page_count = HomePluginPageCount(state->cells);
  while (static_cast<int>(state->pages.size()) < state->page_count) {
    auto *page = lv_obj_create(state->view);
    Clear(page);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(page, state->pages.size() * state->geometry.width, 0);
    lv_obj_set_size(page, state->geometry.width, state->geometry.height);
    lv_obj_add_flag(page, LV_OBJ_FLAG_SNAPPABLE);
    state->pages.push_back(page);
  }
  for (size_t i = 0; i < state->tiles.size(); ++i) {
    auto &tile = state->tiles[i];
    if (!tile.object) { CreateTile(state, i); continue; }
    if (state->dragging && tile.plugin.id == state->selected) continue;
    const auto rect = Bounds(state, state->cells[i]);
    auto *parent = state->pages[state->cells[i].page];
    const bool same_parent = lv_obj_get_parent(tile.object) == parent;
    if (!same_parent) lv_obj_set_parent(tile.object, parent);
    lv_anim_delete(tile.object, nullptr);
    if (animate && same_parent) {
      for (int axis = 0; axis < 2; ++axis) {
        lv_anim_t motion; lv_anim_init(&motion);
        lv_anim_set_var(&motion, tile.object);
        lv_anim_set_values(&motion, axis ? lv_obj_get_y(tile.object) : lv_obj_get_x(tile.object),
                           axis ? rect.y1 : rect.x1);
        lv_anim_set_duration(&motion, 100);
        lv_anim_set_path_cb(&motion, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&motion, axis ? +[](void *o, int32_t p) { lv_obj_set_y(static_cast<lv_obj_t *>(o), p); }
                                           : +[](void *o, int32_t p) { lv_obj_set_x(static_cast<lv_obj_t *>(o), p); });
        lv_anim_start(&motion);
      }
    } else lv_obj_set_pos(tile.object, rect.x1, rect.y1);
  }
  if (!state->dragging) {
    while (static_cast<int>(state->pages.size()) > state->page_count) {
      lv_obj_delete(state->pages.back()); state->pages.pop_back();
    }
    lv_obj_update_layout(state->view);
    lv_obj_scroll_to_x(state->view, std::min(current, state->page_count - 1) * state->geometry.width, LV_ANIM_OFF);
  } else if (current >= state->page_count) {
    lv_obj_scroll_to_x(state->view, (state->page_count - 1) * state->geometry.width, LV_ANIM_OFF);
  }
  ArrangeViewport(state);
  if (static_cast<int>(lv_obj_get_child_count(state->dots)) != state->page_count) {
    lv_obj_clean(state->dots);
    for (int i = 0; i < state->page_count; ++i) {
      auto *dot = lv_obj_create(state->dots); Clear(dot);
      lv_obj_set_size(dot, 14, 14); lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_opa(dot, LV_OPA_70, 0);
      lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    }
  }
  if (state->page_count == 1) lv_obj_add_flag(state->dots, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(state->dots, LV_OBJ_FLAG_HIDDEN);
  UpdateDots(state);
  if (state->editing) Selection(state);
}
void Restore(Grid *state, const std::vector<HomePluginPreference> &before) {
  std::vector<Tile> ordered;
  for (const auto &item : before) {
    const int at = Index(state, item.id);
    if (at < 0) continue;
    auto tile = std::move(state->tiles[at]);
    if (tile.size != item.size && tile.object) {
      lv_obj_delete(tile.object); tile.object = nullptr;
    }
    tile.size = item.size;
    ordered.push_back(std::move(tile));
  }
  state->tiles = std::move(ordered);
  Reflow(state);
}
bool Save(Grid *state, const std::vector<HomePluginPreference> &before) {
  const auto after = Preferences(state);
  if (after.size() == before.size() && std::equal(after.begin(), after.end(), before.begin(),
      [](const auto &a, const auto &b) { return a.id == b.id && a.size == b.size; })) return true;
  if (RecoverySaveHomePluginLayout(after)) return true;
  Restore(state, before);
  Sheet(state->screen, "Layout unavailable", "The Home plugin grid could not be changed.");
  return false;
}
void RemoveSelected(Grid *state) {
  const int index = Index(state, state->selected);
  if (index < 0) return;
  const auto plugin = state->tiles[index].plugin;
  StopEditing(state);
  Sheet(state->screen, "Remove plugin?", plugin.name + "\n\n" +
        i18n::Translate("Remove this plugin from AERA?"), [state, id = plugin.id] {
          plugins::Request request; request.job = plugins::Job::kRemove; request.id = id;
          SetPluginRequest(request);
          state->callback(Action::kRunPluginOperation, state->context);
        });
}
bool OverTrash(Grid *state) {
  lv_area_t area{}; lv_obj_get_coords(state->trash, &area);
  return state->point.x >= area.x1 && state->point.x <= area.x2 &&
         state->point.y >= area.y1 && state->point.y <= area.y2;
}
void DragTick(Grid *state) {
  if (!state->dragging) return;
  lv_obj_set_pos(state->ghost, state->point.x - state->offset.x,
                  state->point.y - state->offset.y);
  lv_obj_set_style_bg_color(state->trash, OverTrash(state) ? kRedSoft : kInset, 0);
  lv_obj_set_style_border_color(state->trash, OverTrash(state) ? kRed : kMainLine, 0);
  lv_area_t area{}; lv_obj_get_coords(state->view, &area);
  if (state->point.y < area.y1 || state->point.y > area.y2 || OverTrash(state)) {
    state->edge_since = 0; state->vertical_since = 0; state->vertical_direction = 0; return;
  }
  const int visible_page = CurrentPage(state);
  auto *page_object = state->pages[visible_page];
  int bottom = 0;
  for (const auto &cell : state->cells)
    if (cell.page == visible_page) bottom = std::max(bottom, Bounds(state, cell).y2 + 1);
  const int scroll = lv_obj_get_scroll_y(page_object);
  const int limit = std::max(0, bottom - lv_obj_get_height(page_object));
  const int vertical = state->point.y < area.y1 + 56 && scroll > 0 ? -1 :
      state->point.y > area.y2 - 56 && scroll < limit ? 1 : 0;
  if (vertical) {
    const uint32_t now = lv_tick_get();
    if (!state->vertical_since || state->vertical_direction != vertical ||
        state->vertical_page != visible_page) {
      state->vertical_since = now; state->vertical_step = now;
    }
    state->vertical_direction = vertical; state->vertical_page = visible_page;
    if (lv_tick_elaps(state->vertical_since) >= 350 && lv_tick_elaps(state->vertical_step) >= 40) {
      state->vertical_step = now;
      lv_obj_scroll_to_y(page_object, std::clamp(scroll + vertical * 16, 0, limit), LV_ANIM_OFF);
    }
  } else {
    state->vertical_since = 0; state->vertical_direction = 0;
  }
  const int direction = state->point.x < area.x1 + 84 ? -1 : state->point.x > area.x2 - 84 ? 1 : 0;
  if (direction) {
    const uint32_t now = lv_tick_get();
    if (state->edge_direction != direction || !state->edge_since) state->edge_since = now;
    state->edge_direction = direction;
    const int page = CurrentPage(state), next = std::clamp(page + direction, 0, state->page_count - 1);
    if (next != page && lv_tick_elaps(state->edge_since) >= 550) {
      lv_obj_scroll_to_x(state->view, next * state->geometry.width, LV_ANIM_OFF);
      state->edge_since = now; UpdateDots(state);
    }
    return;
  }
  state->edge_since = 0; state->edge_direction = 0;
  const int page = CurrentPage(state), from = Index(state, state->selected);
  if (from < 0) return;
  const int x = state->point.x - area.x1;
  const int y = state->point.y - area.y1 + lv_obj_get_scroll_y(page_object);
  bool occupied = false;
  int target = -1;
  for (size_t i = 0; i < state->cells.size(); ++i) {
    if (state->cells[i].page != page) continue;
    const auto r = Bounds(state, state->cells[i]);
    if (x >= r.x1 && x <= r.x2 && y >= r.y1 && y <= r.y2) {
      target = i; occupied = true; break;
    }
  }
  if (!occupied) {
    // Empty cells have no widget to hit. Test insertions against their actual
    // packed position instead of selecting whichever neighbour is closest.
    auto distance = [&](const HomePluginCell &cell) {
      if (cell.page != page) return std::numeric_limits<double>::max();
      const auto box = Bounds(state, cell);
      const double dx = x - std::clamp(x, box.x1, box.x2);
      const double dy = y - std::clamp(y, box.y1, box.y2);
      return dx * dx + dy * dy;
    };
    target = from;
    double best = distance(state->cells[from]);
    auto remaining = Preferences(state);
    const auto selected = remaining[from];
    remaining.erase(remaining.begin() + from);
    for (size_t index = 0; index <= remaining.size(); ++index) {
      if (static_cast<int>(index) == from) continue;
      auto candidate = remaining;
      candidate.insert(candidate.begin() + index, selected);
      const auto cells = PackHomePlugins(candidate, state->geometry.columns,
                                        state->geometry.rows, state->geometry.icons);
      const double score = distance(cells[index]);
      if (score < best) { best = score; target = static_cast<int>(index); }
      if (score == 0) break;
    }
  }
  if (target < 0 || from == target) return;
  auto moved = std::move(state->tiles[from]);
  state->tiles.erase(state->tiles.begin() + from);
  state->tiles.insert(state->tiles.begin() + target, std::move(moved));
  Reflow(state, true);
}
void EndDrag(Grid *state, bool cancel) {
  if (!state->dragging) return;
  const bool remove = !cancel && OverTrash(state);
  state->dragging = false;
  if (kPointerDragOwner == state->view) kPointerDragOwner = nullptr;
  if (state->timer) { lv_timer_delete(state->timer); state->timer = nullptr; }
  if (state->ghost) { lv_obj_delete(state->ghost); state->ghost = nullptr; }
  const int at = Index(state, state->selected);
  if (at >= 0) {
    lv_obj_set_style_opa(state->tiles[at].object, LV_OPA_COVER, 0);
    lv_obj_remove_flag(state->tiles[at].object, LV_OBJ_FLAG_PRESS_LOCK);
  }
  lv_obj_set_scroll_dir(state->view, LV_DIR_HOR);
  lv_obj_set_style_bg_color(state->trash, kInset, 0);
  lv_obj_set_style_border_color(state->trash, kMainLine, 0);
  if (cancel || remove) Restore(state, state->before);
  else { Reflow(state); Save(state, state->before); }
  if (remove) RemoveSelected(state);
}
void StopEditing(Grid *state) {
  if (state->dragging) EndDrag(state, true);
  state->editing = false;
  lv_obj_add_flag(state->toolbar, LV_OBJ_FLAG_HIDDEN);
  ArrangeViewport(state);
  Selection(state);
}
void BeginDrag(Grid *state, const std::string &id, lv_indev_t *input) {
  if (!input || state->dragging) return;
  const int at = Index(state, id); if (at < 0) return;
  state->input = input; state->selected = id; state->before = Preferences(state);
  state->suppress_click = id;
  state->editing = true; state->dragging = true; state->edge_since = 0;
  state->vertical_since = 0; state->vertical_direction = 0; state->vertical_page = -1;
  kPointerDragOwner = state->view;
  lv_obj_remove_flag(state->toolbar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(state->tiles[at].object, LV_OBJ_FLAG_PRESS_LOCK);
  ArrangeViewport(state);
  Selection(state);
  auto *card = state->tiles[at].object;
  lv_obj_update_layout(state->screen);
  lv_area_t rect{}; lv_obj_get_coords(card, &rect);
  lv_indev_get_point(input, &state->point);
  state->offset = {state->point.x - rect.x1, state->point.y - rect.y1};
  lv_obj_add_flag(card, LV_OBJ_FLAG_PRESS_LOCK);
  lv_anim_delete(card, nullptr);
  lv_obj_set_style_opa(card, LV_OPA_30, 0);
  lv_obj_set_scroll_dir(state->view, LV_DIR_NONE);
  state->ghost = state->factory(state->screen, state->tiles[at].plugin, rect.x1, rect.y1,
      lv_area_get_width(&rect), lv_area_get_height(&rect), state->geometry.icons, [] {});
  lv_obj_set_style_opa(state->ghost, LV_OPA_80, 0);
  lv_obj_set_style_border_width(state->ghost, 3, 0);
  lv_obj_set_style_border_color(state->ghost, kAccent, 0);
  auto pass = [](auto &&self, lv_obj_t *object) -> void {
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) self(self, lv_obj_get_child(object, i));
  };
  pass(pass, state->ghost);
  lv_obj_move_foreground(state->toolbar);
  lv_obj_move_foreground(state->ghost);
  state->timer = lv_timer_create([](lv_timer_t *timer) {
    DragTick(static_cast<Grid *>(lv_timer_get_user_data(timer)));
  }, 40, state);
  RecoveryVibrate(Haptic::kTouch);
}
void Press(lv_event_t *event) {
  auto *binding = static_cast<Binding *>(lv_event_get_user_data(event));
  const auto code = lv_event_get_code(event);
  if (code == LV_EVENT_DELETE) { delete binding; return; }
  if (code != LV_EVENT_PRESSED && code != LV_EVENT_LONG_PRESSED &&
      code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
  auto *state = binding->grid;
  if (state->deleting) return;
  if (code == LV_EVENT_PRESSED) state->suppress_click.clear();
  if (code == LV_EVENT_LONG_PRESSED || (code == LV_EVENT_PRESSED && state->editing)) {
    BeginDrag(state, binding->id, lv_indev_active());
  } else if (state->dragging && state->selected == binding->id) {
    if (code == LV_EVENT_PRESSING) {
      lv_indev_get_point(state->input, &state->point); DragTick(state);
    } else if (code == LV_EVENT_RELEASED) EndDrag(state, false);
    else if (code == LV_EVENT_PRESS_LOST) EndDrag(state, true);
  }
}
void FitToolbarButton(lv_obj_t *button) {
  auto *label = lv_obj_get_child(button, 0);
  FitLabelToLines(label, std::max(40, lv_obj_get_width(button) - 40), 1,
      {&lv_font_montserrat_40, &lv_font_montserrat_36, &lv_font_montserrat_32});
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(label);
}
lv_obj_t *ToolbarButton(lv_obj_t *parent, const char *text, Handler handler, bool primary = false) {
  auto *button = Button(parent, text, std::move(handler), primary);
  lv_obj_add_event_cb(button, [](lv_event_t *event) {
    FitToolbarButton(lv_event_get_target_obj(event));
  }, LV_EVENT_SIZE_CHANGED, nullptr);
  return button;
}
void BuildToolbar(Grid *state) {
  state->toolbar = lv_obj_create(state->screen);
  Panel(state->toolbar, 32, kMainSheet);
  const bool landscape = Landscape(state->screen);
  const int width = landscape ? state->geometry.width : std::min(state->geometry.width, 1312);
  const int content_width = width - 40;
  lv_obj_set_size(state->toolbar, width, 292);
  lv_obj_align(state->toolbar, LV_ALIGN_BOTTOM_MID, 0, -18);
  lv_obj_set_style_pad_all(state->toolbar, 20, 0);
  lv_obj_set_user_data(state->toolbar, &kPersistentModalMarker);
  lv_obj_add_event_cb(state->toolbar, [](lv_event_t *event) {
    StopEditing(static_cast<Grid *>(lv_event_get_user_data(event)));
  }, LV_EVENT_CANCEL, state);
  state->title = Label(state->toolbar, "", &lv_font_montserrat_40, kText);
  lv_obj_set_pos(state->title, 12, 24);
  lv_obj_set_width(state->title, content_width - 360);
  lv_label_set_long_mode(state->title, LV_LABEL_LONG_DOT);
  auto *done = ToolbarButton(state->toolbar, "Done", [state] { StopEditing(state); }, true);
  lv_obj_set_size(done, 300, 100); lv_obj_align(done, LV_ALIGN_TOP_RIGHT, 0, 0);
  int count = 0;
  for (size_t i = 0; i < kHomePluginSizes.size(); ++i) {
    const auto &format = kHomePluginSizes[i];
    if (format.width > state->geometry.columns || format.height > state->geometry.rows) continue;
    auto *button = ToolbarButton(state->toolbar, format.label, [state, i] {
      const int at = Index(state, state->selected); if (at < 0 || state->dragging) return;
      const auto before = Preferences(state);
      state->tiles[at].size = static_cast<int>(i);
      lv_obj_delete(state->tiles[at].object); state->tiles[at].object = nullptr;
      Reflow(state); Save(state, before);
      const int selected = Index(state, state->selected);
      if (selected >= 0) {
        const auto &cell = state->cells[selected];
        lv_obj_scroll_to_x(state->view, cell.page * state->geometry.width, LV_ANIM_OFF);
        auto *page = state->pages[cell.page];
        const auto box = Bounds(state, cell);
        const int scroll = lv_obj_get_scroll_y(page);
        const int height = lv_obj_get_height(page);
        if (box.y1 < scroll || lv_area_get_height(&box) > height)
          lv_obj_scroll_to_y(page, box.y1, LV_ANIM_OFF);
        else if (box.y2 >= scroll + height)
          lv_obj_scroll_to_y(page, box.y2 - height + 1, LV_ANIM_OFF);
      }
    });
    const int button_width = (content_width - (landscape ? 680 : 0) - 32) / 3;
    lv_obj_set_pos(button, (count % 3) * (button_width + 16), 120 + (count / 3) * 104);
    lv_obj_set_size(button, button_width, 92);
    state->sizes[i] = button; ++count;
  }
  state->size_rows = (count + 2) / 3;
  state->trash = lv_button_create(state->toolbar);
  Panel(state->trash, 28, kInset);
  Interactive(state->trash, kMainSelected);
  lv_obj_set_size(state->trash, lv_pct(100), 128);
  lv_obj_align(state->trash, LV_ALIGN_BOTTOM_MID, 0, 0);
  if (landscape) {
    lv_obj_set_width(state->trash, 600);
    lv_obj_align(state->trash, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  }
  lv_obj_set_flex_flow(state->trash, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(state->trash, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(state->trash, 24, 0);
  OnClick(state->trash, [state] { RemoveSelected(state); });
  auto *icon_space = lv_obj_create(state->trash); Clear(icon_space);
  lv_obj_set_size(icon_space, 80, 80);
  lv_obj_remove_flag(icon_space, LV_OBJ_FLAG_CLICKABLE);
  auto *icon = Label(icon_space, LV_SYMBOL_TRASH, &lv_font_montserrat_48, kText);
  lv_obj_set_style_transform_scale(icon, 352, 0); lv_obj_center(icon);
  auto *label = Label(state->trash, "Remove", &lv_font_montserrat_40, kText);
  const int label_width = landscape ? 420 : content_width - 180;
  FitLabelToLines(label, label_width, 1,
      {&lv_font_montserrat_40, &lv_font_montserrat_36, &lv_font_montserrat_32});
  lv_obj_set_style_max_width(label, label_width, 0);
  lv_obj_set_width(label, LV_SIZE_CONTENT);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_add_flag(state->toolbar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_height(state->toolbar, landscape ? 292 : 296 + state->size_rows * 104);
}
}  // namespace

void BuildHomePluginGrid(lv_obj_t *screen, const std::vector<plugins::Plugin> &installed,
                        HomePluginGeometry geometry, HomePluginTileFactory factory,
                        std::function<void(const plugins::Plugin &)> launch,
                        ActionCallback callback, void *context) {
  auto *state = new Grid;
  state->screen = screen; state->geometry = geometry;
  state->factory = std::move(factory); state->launch = std::move(launch);
  state->callback = callback; state->context = context;
  std::vector<std::string> ids;
  for (const auto &plugin : installed) if (plugins::IsLaunchable(plugin)) ids.push_back(plugin.id);
  for (const auto &item : OrderHomePlugins(ids, RecoveryHomePluginLayout())) {
    auto plugin = std::find_if(installed.begin(), installed.end(), [&](const auto &p) { return p.id == item.id; });
    state->tiles.push_back({*plugin, item.size, nullptr});
  }
  state->view = lv_obj_create(screen);
  Clear(state->view);
  lv_obj_set_pos(state->view, geometry.x, geometry.y);
  lv_obj_set_size(state->view, geometry.width, geometry.height);
  lv_obj_add_flag(state->view, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(state->view, LV_OBJ_FLAG_SCROLL_ONE);
  lv_obj_add_flag(state->view, LV_OBJ_FLAG_SCROLL_MOMENTUM);
  lv_obj_set_scroll_dir(state->view, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(state->view, LV_SCROLL_SNAP_CENTER);
  lv_obj_remove_flag(state->view, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
  lv_obj_set_scrollbar_mode(state->view, LV_SCROLLBAR_MODE_OFF);
  state->dots = lv_obj_create(screen); Clear(state->dots);
  lv_obj_set_pos(state->dots, geometry.x, geometry.y + geometry.height + 18);
  lv_obj_set_size(state->dots, geometry.width, 40);
  lv_obj_set_flex_flow(state->dots, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(state->dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(state->dots, 14, 0);
  BuildToolbar(state);
  lv_obj_add_event_cb(state->view, [](lv_event_t *event) {
    auto *s = static_cast<Grid *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
      s->deleting = true;
      if (kPointerDragOwner == s->view) kPointerDragOwner = nullptr;
      if (s->timer) lv_timer_delete(s->timer);
      if (s->ghost) lv_obj_delete(s->ghost);
      lv_obj_delete(s->toolbar); lv_obj_delete(s->dots); delete s;
    } else if (lv_event_get_code(event) == LV_EVENT_SCROLL || lv_event_get_code(event) == LV_EVENT_SCROLL_END) {
      UpdateDots(s); lv_obj_invalidate(s->screen);
    }
  }, LV_EVENT_ALL, state);
  Reflow(state);
}
}  // namespace aeraui
