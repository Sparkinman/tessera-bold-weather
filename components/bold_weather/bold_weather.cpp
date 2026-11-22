#include "bold_weather.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include "esphome/core/hal.h"

namespace esphome::bold_weather {

using tessera::Font;
namespace ui = tessera::ui;

// ---- Text and fonts ----

static int text_w(const std::string &text, const lv_font_t *font) {
  lv_point_t size;
  lv_text_get_size(&size, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return size.x;
}
static int ascent(const lv_font_t *font) { return font->line_height - font->base_line; }
// How far the top of a capital is below the line's top, and how tall it is: the layout centres what shows.
static void cap_box(const lv_font_t *font, int &top, int &h) {
  lv_font_glyph_dsc_t g{};
  if (lv_font_get_glyph_dsc(font, &g, 'H', 0) && g.box_h > 0) {
    h = g.box_h;
    top = ascent(font) - g.box_h - g.ofs_y;
  } else {
    h = font->line_height;
    top = 0;
  }
}
static int cap_h(const lv_font_t *font) {
  int top, h;
  cap_box(font, top, h);
  return h;
}

// The fonts of a view, largest first: the plugin's bold Roboto, then the screen's bold name font.
static std::vector<const lv_font_t *> font_list(const BoldWeather *plugin) {
  std::vector<const lv_font_t *> out;
  for (auto it = plugin->fonts().rbegin(); it != plugin->fonts().rend(); ++it) out.push_back((*it)->get_lv_font());
  out.push_back(ui::font(Font::TITLE));
  return out;
}
// The largest font whose capitals fit `max_h` and whose text fits `max_w`.
static const lv_font_t *fit(const std::vector<const lv_font_t *> &fonts, const std::string &text, int max_w, int max_h) {
  for (const lv_font_t *f : fonts)
    if (cap_h(f) <= max_h && text_w(text, f) <= max_w) return f;
  return fonts.back();
}
// One line of words in a font, its capitals centred in the band y0..y0+h; x0 is its left, or its right with `right`.
static void place(lv_obj_t *label, const std::string &text, const lv_font_t *f, int x0, int y0, int h,
                  bool right = false, bool centre_w = false, int band_w = 0) {
  lv_obj_set_style_text_font(label, f, 0);
  ui::set_text(label, text);
  int ctop, ch;
  cap_box(f, ctop, ch);
  const int tw = text_w(text, f);
  lv_obj_set_size(label, tw + 2, f->line_height);
  int x = right ? x0 - tw : centre_w ? x0 + std::max(0, (band_w - tw) / 2) : x0;
  lv_obj_set_pos(label, x, y0 + std::max(0, (h - ch) / 2) - ctop);
  lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
}
static void hide(lv_obj_t *o) {
  if (o) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// ---- Weather words and numbers ----

static int32_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
static int weekday_of(int32_t civil) { return static_cast<int>(((civil % 7) + 11) % 7); }  // 0 is Sunday

static uint32_t icon_for(const std::string &c) {
  if (c == "sunny") return 0xF0599;
  if (c == "clear-night") return 0xF0594;
  if (c == "cloudy") return 0xF0590;
  if (c == "partlycloudy") return 0xF0595;
  if (c == "rainy") return 0xF0597;
  if (c == "pouring") return 0xF0596;
  if (c == "snowy") return 0xF0598;
  if (c == "snowy-rainy") return 0xF067F;
  if (c == "fog") return 0xF0591;
  if (c == "hail") return 0xF0592;
  if (c == "lightning") return 0xF0593;
  if (c == "lightning-rainy") return 0xF067E;
  if (c == "windy" || c == "windy-variant") return 0xF059D;
  return 0xF05D6;
}
static std::string degrees(float v) {
  if (std::isnan(v)) return "-";
  char buf[12];
  snprintf(buf, sizeof buf, "%d\xC2\xB0", static_cast<int>(std::lround(v)));
  return buf;
}

// ---- The plugin: one forecast per weather entity, shared ----

BoldWeather::Entry &BoldWeather::entry(const std::string &entity) {
  for (auto &e : entries_)
    if (e.entity == entity) return e;
  entries_.push_back(Entry{});
  entries_.back().entity = entity;
  return entries_.back();
}
const BoldWeather::Entry *BoldWeather::find(const std::string &entity) const {
  for (const auto &e : entries_)
    if (e.entity == entity) return &e;
  return nullptr;
}
void BoldWeather::forget(Listener *l) {
  listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), l), listeners_.end());
}
void BoldWeather::ask(const std::string &entity, bool force) {
  if (entity.empty()) return;
  Entry &e = entry(entity);
  const uint32_t now = millis() / 1000;
  const uint32_t wait = force ? 60 : 600;
  if (e.asked_s && now - e.asked_s < wait) return;
  JsonDocument request;
  request["ask"] = "call_service:weather.get_forecasts";
  request["data"]["entity_id"] = entity;
  request["data"]["type"] = "daily";
  e.re = tessera::send(this, request.as<JsonObjectConst>());
  if (e.re) e.asked_s = now;
}
// The hourly forecast, for the card's hours (the answer holds about the next fifteen); asked once the daily one is in,
// so the two never wait at once.
static uint32_t ask_hourly(BoldWeather *plugin, const std::string &entity) {
  JsonDocument hourly;
  hourly["ask"] = "call_service:weather.get_forecasts";
  hourly["data"]["entity_id"] = entity;
  hourly["data"]["type"] = "hourly";
  return tessera::send(plugin, hourly.as<JsonObjectConst>());
}
const std::vector<Hour> *BoldWeather::hours(const std::string &entity) const {
  const Entry *e = find(entity);
  return e && !e->hours.empty() ? &e->hours : nullptr;
}
// "2026-11-22T06:00:00+00:00" as seconds since 1970 (UTC).
static uint32_t epoch_of(const char *when) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, oh = 0, om = 0;
  char sign = '+';
  const int n = sscanf(when, "%d-%d-%dT%d:%d:%d%c%d:%d", &y, &mo, &d, &h, &mi, &s, &sign, &oh, &om);
  if (n < 6) return 0;
  int64_t t = static_cast<int64_t>(days_from_civil(y, mo, d)) * 86400 + h * 3600 + mi * 60 + s;
  if (n >= 8 && (sign == '+' || sign == '-')) t -= (sign == '-' ? -1 : 1) * (oh * 3600 + om * 60);
  return t > 0 ? static_cast<uint32_t>(t) : 0;
}
void BoldWeather::set_now(const std::string &entity, JsonObjectConst data) {
  Entry &e = entry(entity);
  JsonObjectConst a = data["attributes"];
  Now &n = e.now;
  const char *state = data["state"] | "";
  if (*state) n.condition = state;
  n.temp = a["temperature"] | n.temp;
  n.feels = a["apparent_temperature"] | n.feels;
  n.wind = a["wind_speed"] | n.wind;
  n.bearing = a["wind_bearing"] | n.bearing;
  n.pressure = a["pressure"] | n.pressure;
  n.dew = a["dew_point"] | n.dew;
  n.uv = a["uv_index"] | n.uv;
  const float clouds = a["cloud_coverage"] | NAN;
  if (!std::isnan(clouds)) n.clouds = static_cast<int>(std::lround(clouds));
  const float humidity = a["humidity"] | NAN;
  if (!std::isnan(humidity)) n.humidity = static_cast<int>(std::lround(humidity));
  const char *pu = a["pressure_unit"] | "";
  if (*pu) n.pressure_unit = pu;
  const char *wu = a["wind_speed_unit"] | "";
  if (*wu) e.wind_unit = wu;
  const char *ru = a["precipitation_unit"] | "";
  if (*ru) e.rain_unit = ru;
  e.have_now = true;
}
const BoldWeather::Now *BoldWeather::now(const std::string &entity) const {
  const Entry *e = find(entity);
  return e && e->have_now ? &e->now : nullptr;
}
const std::vector<Day> *BoldWeather::days(const std::string &entity) const {
  const Entry *e = find(entity);
  return e && e->have ? &e->days : nullptr;
}
void BoldWeather::set_units(const std::string &entity, const std::string &wind, const std::string &rain) {
  Entry &e = entry(entity);
  e.wind_unit = wind;
  e.rain_unit = rain;
}
std::string BoldWeather::wind_unit(const std::string &entity) const {
  const Entry *e = find(entity);
  return e ? e->wind_unit : "";
}
std::string BoldWeather::rain_unit(const std::string &entity) const {
  const Entry *e = find(entity);
  return e ? e->rain_unit : "";
}

// The answer: {"re", "ok", "result": {"weather.x": {"forecast": [{"datetime", "condition", "temperature", "templow",
// "precipitation_probability", "precipitation", "wind_speed", "humidity"}, ...]}}}.
void BoldWeather::on_message(JsonObjectConst message) {
  const uint32_t re = message["re"] | 0u;
  for (auto &e : entries_) {
    if (re && e.hours_re == re) {
      if (!(message["ok"] | false)) return;
      e.hours.clear();
      for (JsonObjectConst f : message["result"][e.entity]["forecast"].as<JsonArrayConst>()) {
        Hour h;
        h.epoch = epoch_of(f["datetime"] | "");
        if (!h.epoch) continue;
        h.condition = f["condition"] | "";
        h.temp = f["temperature"] | NAN;
        h.rain = f["precipitation"] | NAN;
        e.hours.push_back(h);
        if (e.hours.size() >= 48) break;
      }
      const std::string entity = e.entity;
      for (Listener *l : std::vector<Listener *>(listeners_)) l->on_forecast(entity);
      tessera::refresh();
      return;
    }
    if (!re || e.re != re) continue;
    if (!(message["ok"] | false)) {
      e.asked_s = 0;  // ask again on the next try
      return;
    }
    e.days.clear();
    for (JsonObjectConst f : message["result"][e.entity]["forecast"].as<JsonArrayConst>()) {
      Day d;
      int y = 0, m = 0, day = 0;
      const char *when = f["datetime"] | "";
      if (sscanf(when, "%d-%d-%d", &y, &m, &day) != 3) continue;
      d.civil = days_from_civil(y, m, day);
      d.condition = f["condition"] | "";
      d.high = f["temperature"] | NAN;
      d.low = f["templow"] | NAN;
      d.chance = f["precipitation_probability"] | -1;
      d.rain = f["precipitation"] | NAN;
      d.wind = f["wind_speed"] | NAN;
      d.humidity = static_cast<int>(std::lround(f["humidity"] | -1.0f));
      d.bearing = f["wind_bearing"] | NAN;
      d.uv = f["uv_index"] | NAN;
      e.days.push_back(d);
      if (e.days.size() >= 7) break;
    }
    e.have = true;
    const std::string entity = e.entity;
    for (Listener *l : std::vector<Listener *>(listeners_)) l->on_forecast(entity);
    tessera::refresh();
    entry(entity).hours_re = ask_hourly(this, entity);
    return;
  }
}

// ---- The tile ----

// The weather now on top (its icon and word at the left, the temperature large at the right) and the coming days in
// columns under it (the day, the high large, the low smaller), as many as fit. A tap opens the week card.
class ForecastTile : public tessera::Tile, public BoldWeather::Listener {
 public:
  explicit ForecastTile(BoldWeather *plugin) : plugin_(plugin) {}
  ~ForecastTile() override { plugin_->forget(this); }

  void create(const tessera::TileContext &c) override {
    w_ = c.width;
    h_ = c.height;
    tile_ = c.tile;
    entity_ = c.entity ? c.entity : "";
    const std::string label = c.options["label"] | "";
    name_text_ = !label.empty() ? label : (c.name ? c.name : "");
    fonts_ = font_list(plugin_);
    gap_ = ui::px(ui::large() ? 8 : 5);
    name_ = ui::label(c.parent, Font::TITLE, theme::INK_SOFT);
    name_h_ = ui::line_height(Font::TITLE);
    icon_ = ui::label(c.parent, Font::ICON, theme::INK);
    cond_ = ui::label(c.parent, Font::TITLE, theme::INK);
    temp_ = ui::label(c.parent, Font::TITLE, theme::INK);
    // Where the weather now and the coming days go: the days under the weather now on a tall enough tile, else at
    // its right on a wide enough one (the name and weather now in the left third), else only the weather now.
    const int top = name_h_ + gap_;
    now_x_ = 0;
    now_y_ = top;
    now_w_ = w_;
    days_x_ = 0;
    days_w_ = w_;
    if (h_ >= w_ * 40 / 100 && h_ >= ui::px(150)) {
      days_h_ = h_ * 45 / 100;
      days_y_ = h_ - days_h_;
      now_h_ = days_y_ - gap_ - top;
    } else if (w_ >= ui::px(200)) {
      now_w_ = w_ * 36 / 100;
      now_h_ = h_ - top;
      days_x_ = now_w_ + gap_;
      days_w_ = w_ - days_x_;
      days_y_ = 0;
      days_h_ = h_;
    } else {
      days_h_ = 0;
      now_h_ = h_ - top;
    }
    lv_obj_set_size(name_, now_w_, name_h_);
    lv_obj_set_pos(name_, 0, 0);
    if (!name_text_.empty()) ui::set_text(name_, name_text_);
    for (auto &col : cols_) {
      col.day = ui::label(c.parent, Font::TITLE, theme::INK_SOFT);
      col.icon = ui::label(c.parent, Font::ICON_SMALL, theme::INK_SOFT);
      col.high = ui::label(c.parent, Font::TITLE, theme::INK);
      col.low = ui::label(c.parent, Font::TITLE, theme::MUTED);
      hide(col.day), hide(col.icon), hide(col.high), hide(col.low);
    }
    plugin_->listen(this);
    plugin_->ask(entity_);
  }

  void on_state(JsonObjectConst data) override {
    if (name_text_.empty()) ui::set_text(name_, data["name"] | "");
    condition_ = data["state"] | "";
    temperature_ = data["attributes"]["temperature"] | NAN;
    plugin_->set_units(entity_, data["attributes"]["wind_speed_unit"] | "", data["attributes"]["precipitation_unit"] | "");
    plugin_->set_now(entity_, data);
    paint_now();
  }

  void on_forecast(const std::string &entity) override {
    if (entity == entity_) paint_days();
  }

  void on_tick(uint32_t epoch) override {
    if (epoch) {
      const tessera::LocalTime t = tessera::local_time(epoch);
      const int32_t today = days_from_civil(t.year, t.month, t.day);
      if (today != today_) {
        today_ = today;
        paint_days();
      }
    }
    // A fresh forecast about every half hour while the tile shows.
    if (++ticks_ % 1800 == 0) plugin_->ask(entity_);
  }

  void on_tap() override {
    tessera::open_card(plugin_->plugin_id(), "week", entity_, tile_, name_text_);
  }

  void on_theme() override {
    ui::set_color(name_, theme::INK_SOFT);
    ui::set_color(icon_, theme::INK);
    ui::set_color(cond_, theme::INK);
    ui::set_color(temp_, theme::INK);
    for (auto &col : cols_) {
      ui::set_color(col.day, theme::INK_SOFT);
      ui::set_color(col.icon, theme::INK_SOFT);
      ui::set_color(col.high, theme::INK);
      ui::set_color(col.low, theme::MUTED);
    }
  }

 private:
  struct Col {
    lv_obj_t *day{}, *icon{}, *high{}, *low{};
  };

  std::string word(const std::string &condition) const {
    if (condition.empty()) return "";
    std::string key = "c_" + condition;
    std::replace(key.begin(), key.end(), '-', '_');
    return plugin_->text(key.c_str());
  }

  void paint_now() {
    const int top = now_y_, now_h = now_h_;
    if (now_h <= 0) return;
    const int icon_w = ui::line_height(Font::ICON);
    ui::set_text(icon_, ui::icon(icon_for(condition_)));
    lv_obj_set_size(icon_, icon_w, ui::line_height(Font::ICON));
    const std::string t = degrees(temperature_);
    const std::string wd = word(condition_);
    if (now_w_ < w_) {
      // Beside the days: the icon and temperature on one line, the word under them.
      const int line_h = wd.empty() ? now_h : now_h * 62 / 100;
      const lv_font_t *tf = fit(fonts_, t, now_w_ - icon_w - gap_, line_h * 80 / 100);
      lv_obj_set_pos(icon_, now_x_, top + (line_h - ui::line_height(Font::ICON)) / 2);
      place(temp_, t, tf, now_x_ + icon_w + gap_, top, line_h);
      if (!wd.empty()) {
        place(cond_, wd, fit(fonts_, wd, now_w_, (now_h - line_h) * 60 / 100), now_x_, top + line_h, now_h - line_h);
      } else {
        hide(cond_);
      }
      return;
    }
    // The temperature at the right, as large as half the tile's width and the band's height allow.
    const lv_font_t *tf = fit(fonts_, t, w_ * 48 / 100, now_h * 85 / 100);
    place(temp_, t, tf, w_, top, now_h, true);
    // The icon and the word at the left.
    lv_obj_set_pos(icon_, 0, top + (now_h - ui::line_height(Font::ICON)) / 2);
    const int left_w = w_ - text_w(t, tf) - icon_w - 3 * gap_;
    if (left_w > 0 && !wd.empty()) {
      place(cond_, wd, fit(fonts_, wd, left_w, now_h * 40 / 100), icon_w + gap_, top, now_h);
    } else {
      hide(cond_);
    }
  }

  void paint_days() {
    for (auto &col : cols_) hide(col.day), hide(col.icon), hide(col.high), hide(col.low);
    const std::vector<Day> *days = plugin_->days(entity_);
    if (!days_h_ || !days || days->empty()) return;
    // From today on; as many columns as fit at about a column per 110 px.
    std::vector<const Day *> shown;
    for (const Day &d : *days)
      if (!today_ || d.civil >= today_) shown.push_back(&d);
    const int n = std::min<int>({static_cast<int>(shown.size()), 7, std::max(1, days_w_ / ui::px(80))});
    if (n <= 0) return;
    const int col_w = days_w_ / n;
    const int y0 = days_y_;
    const int day_h = ui::line_height(Font::TITLE);
    // The icon only where it leaves the temperatures room enough.
    const int icon_h = days_h_ - day_h - ui::line_height(Font::ICON_SMALL) >= ui::px(60) ? ui::line_height(Font::ICON_SMALL) : 0;
    const int rest = days_h_ - day_h - icon_h;
    for (int i = 0; i < n; ++i) {
      const Day &d = *shown[i];
      Col &col = cols_[i];
      const int x = days_x_ + i * col_w;
      const std::string name = d.civil == today_ ? plugin_->text("today") : plugin_->text(SHORT[weekday_of(d.civil)]);
      place(col.day, name, fit(fonts_, name, col_w * 92 / 100, day_h * 70 / 100), x, y0, day_h, false, true, col_w);
      if (icon_h) {
        ui::set_text(col.icon, ui::icon(icon_for(d.condition)));
        lv_obj_set_size(col.icon, col_w, icon_h);
        lv_obj_set_style_text_align(col.icon, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(col.icon, x, y0 + day_h);
        lv_obj_remove_flag(col.icon, LV_OBJ_FLAG_HIDDEN);
      }
      const std::string hi = degrees(d.high), lo = degrees(d.low);
      const lv_font_t *hf = fit(fonts_, hi, col_w * 90 / 100, rest * 45 / 100);
      place(col.high, hi, hf, x, y0 + day_h + icon_h, rest * 60 / 100, false, true, col_w);
      const lv_font_t *lf = fit(fonts_, lo, col_w * 80 / 100, rest * 28 / 100);
      place(col.low, lo, lf, x, y0 + day_h + icon_h + rest * 60 / 100, rest * 40 / 100, false, true, col_w);
    }
  }

  static constexpr const char *SHORT[7] = {"s0", "s1", "s2", "s3", "s4", "s5", "s6"};

  BoldWeather *plugin_;
  int w_ = 0, h_ = 0, tile_ = -1, gap_ = 5, name_h_ = 0, days_h_ = 0;
  int now_x_ = 0, now_y_ = 0, now_w_ = 0, now_h_ = 0, days_x_ = 0, days_y_ = 0, days_w_ = 0;
  int32_t today_ = 0;
  uint32_t ticks_ = 0;
  float temperature_ = NAN;
  std::string entity_, name_text_, condition_;
  std::vector<const lv_font_t *> fonts_;
  lv_obj_t *name_{}, *icon_{}, *cond_{}, *temp_{};
  Col cols_[7];
};
constexpr const char *ForecastTile::SHORT[7];

// ---- The week card ----

static const char *const COMPASS[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
static int compass_of(float bearing) {
  if (std::isnan(bearing)) return -1;
  float b = std::fmod(bearing, 360.0f);
  if (b < 0) b += 360.0f;
  return static_cast<int>((b + 22.5f) / 45.0f) % 8;
}
// Wind in mph whatever the entity's unit, for the words (calm, light, breezy, windy).
static float as_mph(float v, const std::string &unit) {
  if (unit == "km/h") return v / 1.609f;
  if (unit == "m/s") return v * 2.237f;
  if (unit == "kn") return v * 1.151f;
  return v;
}
// Several lines of words, wrapped to max_w and centred, in the largest font whose block fits max_h.
static void place_wrapped(lv_obj_t *label, const std::string &text, const std::vector<const lv_font_t *> &fonts, int x0,
                          int y0, int max_w, int max_h) {
  // The fonts largest first, then the screen's smaller text fonts, so a long forecast still fits whole.
  std::vector<const lv_font_t *> all = fonts;
  all.push_back(ui::font(Font::BODY_LARGE));
  all.push_back(ui::font(Font::BODY));
  const lv_font_t *chosen = all.back();
  lv_point_t size{};
  const int wrap_w = max_w - 8;  // a little slack, so the label's own wrapping never needs a line more than measured
  for (const lv_font_t *f : all) {
    lv_text_get_size(&size, text.c_str(), f, 0, f->line_height / 8, wrap_w, LV_TEXT_FLAG_NONE);
    if (size.y <= max_h && size.x <= wrap_w) {
      chosen = f;
      break;
    }
  }
  lv_text_get_size(&size, text.c_str(), chosen, 0, chosen->line_height / 8, wrap_w, LV_TEXT_FLAG_NONE);
  lv_obj_set_style_text_font(label, chosen, 0);
  lv_obj_set_style_text_line_space(label, chosen->line_height / 8, 0);
  lv_label_set_long_mode(label, static_cast<lv_label_long_mode_t>(0));  // wrap
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_size(label, max_w, static_cast<int>(size.y) + chosen->line_height / 2);
  ui::set_text(label, text);
  lv_obj_set_pos(label, x0, y0 + std::max(0, (max_h - static_cast<int>(size.y)) / 2));
  lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
}

// The card the tile opens. At the top the weather now: the icon and word, the temperature large, and the humidity,
// wind, pressure, dew point, UV and clouds. Under it the coming hours (time, icon, temperature), then a row per day
// (day, icon, word, rain or wind, high and low). A tap on a day shows it in full: the high and low large, a short
// forecast in words, and the rain, wind, humidity and UV, with that day's hours where the hourly forecast reaches.
class WeekCard : public tessera::Card, public BoldWeather::Listener {
 public:
  explicit WeekCard(BoldWeather *plugin) : plugin_(plugin) {}
  ~WeekCard() override { plugin_->forget(this); }

  void open(const tessera::CardContext &c) override {
    w_ = c.width;
    h_ = c.height;
    entity_ = c.entity ? c.entity : "";
    fonts_ = font_list(plugin_);
    pad_ = ui::px(14);
    n_icon_ = ui::label(c.parent, Font::ICON, theme::INK);
    n_word_ = ui::label(c.parent, Font::TITLE, theme::INK_SOFT);
    n_temp_ = ui::label(c.parent, Font::TITLE, theme::INK);
    for (auto &f : facts_) {
      f.key = ui::label(c.parent, Font::BODY, theme::MUTED);
      f.value = ui::label(c.parent, Font::TITLE, theme::INK);
      hide(f.key), hide(f.value);
    }
    for (auto &hc : hours_) {
      hc.time = ui::label(c.parent, Font::BODY, theme::MUTED);
      hc.icon = ui::label(c.parent, Font::ICON_SMALL, theme::INK_SOFT);
      hc.temp = ui::label(c.parent, Font::TITLE, theme::INK);
      hide(hc.time), hide(hc.icon), hide(hc.temp);
    }
    for (auto &r : rows_) {
      r.day = ui::label(c.parent, Font::TITLE, theme::INK);
      r.icon = ui::label(c.parent, Font::ICON, theme::INK);
      r.word = ui::label(c.parent, Font::TITLE, theme::INK_SOFT);
      r.extra = ui::label(c.parent, Font::TITLE, theme::MUTED);
      r.temps = ui::label(c.parent, Font::TITLE, theme::INK);
      r.line = ui::block(c.parent, theme::LINE);
      hide(r.day), hide(r.icon), hide(r.word), hide(r.extra), hide(r.temps), hide(r.line);
    }
    rule1_ = ui::block(c.parent, theme::LINE);
    rule2_ = ui::block(c.parent, theme::LINE);
    d_day_ = ui::label(c.parent, Font::TITLE, theme::INK);
    d_icon_ = ui::label(c.parent, Font::ICON, theme::INK);
    d_word_ = ui::label(c.parent, Font::TITLE, theme::INK_SOFT);
    d_high_ = ui::label(c.parent, Font::TITLE, theme::INK);
    d_low_ = ui::label(c.parent, Font::TITLE, theme::MUTED);
    d_text_ = ui::label(c.parent, Font::TITLE, theme::INK_SOFT);
    hide(rule1_), hide(rule2_), hide(d_day_), hide(d_icon_), hide(d_word_), hide(d_high_), hide(d_low_), hide(d_text_);
    note_ = ui::label(c.parent, Font::TITLE, theme::MUTED);
    lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(note_, w_, ui::line_height(Font::TITLE));
    lv_obj_set_pos(note_, 0, (h_ - ui::line_height(Font::TITLE)) / 2);
    ui::set_text(note_, txt("loading"));
    plugin_->listen(this);
    plugin_->ask(entity_, true);
    paint();
  }

  void on_state(JsonObjectConst data) override {
    plugin_->set_now(entity_, data);
    if (detail_ < 0) paint();
  }

  void on_forecast(const std::string &entity) override {
    if (entity == entity_) paint();
  }

  void on_tick(uint32_t epoch) override {
    if (!epoch) return;
    epoch_ = epoch;
    const tessera::LocalTime t = tessera::local_time(epoch);
    const int32_t today = days_from_civil(t.year, t.month, t.day);
    if (today != today_ || t.hour != hour_) {
      today_ = today;
      hour_ = t.hour;
      paint();
    }
  }

  void on_tap() override {
    if (detail_ >= 0) {
      detail_ = -1;
      paint();
      return;
    }
    if (!row_h_ || shown_.empty()) return;
    const int y = tessera::tap_point().y - rows_y_;
    if (y < 0) return;
    const int row = y / row_h_;
    if (row >= 0 && row < static_cast<int>(shown_.size()) && row < rows_n_) {
      detail_ = row;
      paint();
    }
  }

  bool on_back() override {
    if (detail_ < 0) return false;
    detail_ = -1;
    paint();
    return true;
  }

 private:
  struct Fact {
    lv_obj_t *key{}, *value{};
  };
  struct HourCol {
    lv_obj_t *time{}, *icon{}, *temp{};
  };
  struct Row {
    lv_obj_t *day{}, *icon{}, *word{}, *extra{}, *temps{}, *line{};
  };
  static constexpr int ROWS = 7, HOURS = 8, FACTS = 6;

  std::string txt(const char *key) const { return plugin_->text(key); }

  std::string word(const std::string &condition) const {
    if (condition.empty()) return "";
    std::string key = "c_" + condition;
    std::replace(key.begin(), key.end(), '-', '_');
    return txt(key.c_str());
  }
  std::string day_name(const Day &d) const {
    static const char *const LONG[7] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6"};
    if (d.civil == today_) return txt("today");
    if (d.civil == today_ + 1) return txt("tomorrow");
    return txt(LONG[weekday_of(d.civil)]);
  }
  std::string wind_text(float speed, float bearing) const {
    if (std::isnan(speed)) return "-";
    char buf[32];
    const int dir = compass_of(bearing);
    snprintf(buf, sizeof buf, "%s%s%d %s", dir >= 0 ? COMPASS[dir] : "", dir >= 0 ? " " : "",
             static_cast<int>(std::lround(speed)), plugin_->wind_unit(entity_).c_str());
    return buf;
  }
  std::string rain_text(float rain) const {
    if (std::isnan(rain)) return "-";
    char buf[24];
    const std::string unit = plugin_->rain_unit(entity_);
    if (unit == "in") snprintf(buf, sizeof buf, "%.2f in", rain);
    else snprintf(buf, sizeof buf, "%.1f %s", rain, unit.c_str());
    return buf;
  }
  static std::string hour_text(int hour) {
    char buf[8];
    const int h12 = hour % 12 == 0 ? 12 : hour % 12;
    snprintf(buf, sizeof buf, "%d %s", h12, hour < 12 ? "AM" : "PM");
    return buf;
  }

  // A short forecast in words: "Partly cloudy, a high of 36 and a low of 24. Light east wind, about 6 mph. Humidity
  // near 58%. Dry." from the numbers the daily forecast has.
  std::string sentence(const Day &d) const {
    std::string s;
    char buf[96];
    const std::string wd = word(d.condition);
    if (!std::isnan(d.high) && !std::isnan(d.low)) {
      snprintf(buf, sizeof buf, txt("s_temps").c_str(), static_cast<int>(std::lround(d.high)),
               static_cast<int>(std::lround(d.low)));
      s = (wd.empty() ? std::string() : wd + ", ") + buf;
    } else {
      s = wd + ".";
    }
    if (!std::isnan(d.wind)) {
      const float mph = as_mph(d.wind, plugin_->wind_unit(entity_));
      const char *strength = mph < 4 ? "s_calm" : mph < 12 ? "s_light" : mph < 20 ? "s_breezy" : mph < 30 ? "s_windy" : "s_gusty";
      const int dir = compass_of(d.bearing);
      if (mph < 4) {
        s += " " + txt("s_calm");
      } else {
        static const char *const FROM[8] = {"w_n", "w_ne", "w_e", "w_se", "w_s", "w_sw", "w_w", "w_nw"};
        snprintf(buf, sizeof buf, txt("s_wind").c_str(), txt(strength).c_str(),
                 dir >= 0 ? txt(FROM[dir]).c_str() : "", static_cast<int>(std::lround(d.wind)),
                 plugin_->wind_unit(entity_).c_str());
        s += " " + std::string(buf);
      }
    }
    const bool snow = d.condition == "snowy" || d.condition == "snowy-rainy";
    if (d.chance > 0) {
      snprintf(buf, sizeof buf, txt(snow ? "s_chance_snow" : "s_chance").c_str(), d.chance);
      s += " " + std::string(buf);
    } else if (!std::isnan(d.rain) && d.rain > 0.004f) {
      snprintf(buf, sizeof buf, txt(snow ? "s_snow" : "s_rain").c_str(), rain_text(d.rain).c_str());
      s += " " + std::string(buf);
    } else if (!std::isnan(d.rain)) {
      s += " " + txt("s_dry");
    }
    if (!std::isnan(d.uv) && d.uv >= 6) s += " " + txt("s_uv_high");
    return s;
  }

  void hide_all() {
    hide(n_icon_), hide(n_word_), hide(n_temp_), hide(rule1_), hide(rule2_);
    for (auto &f : facts_) hide(f.key), hide(f.value);
    for (auto &hc : hours_) hide(hc.time), hide(hc.icon), hide(hc.temp);
    for (auto &r : rows_) hide(r.day), hide(r.icon), hide(r.word), hide(r.extra), hide(r.temps), hide(r.line);
    hide(d_day_), hide(d_icon_), hide(d_word_), hide(d_high_), hide(d_low_), hide(d_text_);
  }

  void paint() {
    hide_all();
    shown_.clear();
    const std::vector<Day> *days = plugin_->days(entity_);
    if (!days || days->empty()) {
      lv_obj_remove_flag(note_, LV_OBJ_FLAG_HIDDEN);
      return;
    }
    hide(note_);
    for (const Day &d : *days)
      if (!today_ || d.civil >= today_) shown_.push_back(d);
    if (detail_ >= static_cast<int>(shown_.size())) detail_ = -1;
    if (detail_ >= 0) {
      paint_detail(shown_[detail_]);
    } else {
      paint_main();
    }
  }

  // The hours from now on (or of one day), at most `max`, spread over what the hourly forecast has.
  std::vector<const Hour *> pick_hours(int32_t civil, int max) const {
    std::vector<const Hour *> out;
    const std::vector<Hour> *hours = plugin_->hours(entity_);
    if (!hours) return out;
    std::vector<const Hour *> all;
    for (const Hour &h : *hours) {
      if (epoch_ && h.epoch + 3600 <= epoch_) continue;
      if (civil) {
        const tessera::LocalTime t = tessera::local_time(h.epoch);
        if (days_from_civil(t.year, t.month, t.day) != civil) continue;
      }
      all.push_back(&h);
    }
    if (all.empty()) return out;
    const int step = std::max(1, static_cast<int>((all.size() + max - 1) / max));
    for (size_t i = 0; i < all.size() && static_cast<int>(out.size()) < max; i += step) out.push_back(all[i]);
    return out;
  }

  void paint_hours(const std::vector<const Hour *> &hs, int y0, int band_h) {
    if (hs.empty()) return;
    const int n = static_cast<int>(hs.size());
    const int col_w = (w_ - 2 * pad_) / std::max(n, 6);
    const int x_start = pad_ + ((w_ - 2 * pad_) - col_w * n) / 2;
    const int time_h = ui::line_height(Font::BODY), icon_h = ui::line_height(Font::ICON_SMALL);
    const int temp_h = std::max(ui::px(20), band_h - time_h - icon_h);
    for (int i = 0; i < n; ++i) {
      const Hour &h = *hs[i];
      HourCol &hc = hours_[i];
      const int x = x_start + i * col_w;
      const tessera::LocalTime t = tessera::local_time(h.epoch);
      place(hc.time, hour_text(t.hour), ui::font(Font::BODY), x, y0, time_h, false, true, col_w);
      ui::set_text(hc.icon, ui::icon(icon_for(h.condition)));
      lv_obj_set_size(hc.icon, col_w, icon_h);
      lv_obj_set_style_text_align(hc.icon, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(hc.icon, x, y0 + time_h);
      lv_obj_remove_flag(hc.icon, LV_OBJ_FLAG_HIDDEN);
      const std::string tt = degrees(h.temp);
      place(hc.temp, tt, fit(fonts_, "100\xC2\xB0", col_w * 80 / 100, temp_h * 70 / 100), x, y0 + time_h + icon_h, temp_h,
            false, true, col_w);
    }
  }

  void paint_facts(const std::vector<std::pair<std::string, std::string>> &facts, int x0, int y0, int w, int h, int cols) {
    const int n = std::min<int>(facts.size(), FACTS);
    if (!n) return;
    const int rows = (n + cols - 1) / cols;
    const int cell_w = w / cols, cell_h = h / rows;
    const int key_h = ui::line_height(Font::BODY);
    // One font for every value, so they line up: the largest that fits the longest.
    std::string longest;
    for (int i = 0; i < n; ++i)
      if (facts[i].second.size() > longest.size()) longest = facts[i].second;
    const lv_font_t *vf = fit(fonts_, longest, cell_w * 90 / 100, (cell_h - key_h) * 60 / 100);
    for (int i = 0; i < n; ++i) {
      const int x = x0 + (i % cols) * cell_w, y = y0 + (i / cols) * cell_h;
      place(facts_[i].key, facts[i].first, ui::font(Font::BODY), x, y, key_h, false, true, cell_w);
      place(facts_[i].value, facts[i].second, vf, x, y + key_h, cell_h - key_h, false, true, cell_w);
    }
  }

  void rule(lv_obj_t *line, int y) {
    lv_obj_set_size(line, w_ - 2 * pad_, std::max(1, ui::px(1)));
    lv_obj_set_pos(line, pad_, y);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_HIDDEN);
  }

  void paint_main() {
    const BoldWeather::Now *now = plugin_->now(entity_);
    // The bands: the weather now in one strip of 20%, the days the rest (the hours are on today's page).
    const int now_h = now ? h_ * 20 / 100 : 0;
    rows_y_ = now_h;
    // The weather now: the icon, the temperature large and the word at the left; feels like, humidity, wind and UV
    // at the right.
    if (now) {
      const int left_w = w_ * 46 / 100;
      const int icon_w = ui::line_height(Font::ICON);
      ui::set_text(n_icon_, ui::icon(icon_for(now->condition)));
      lv_obj_set_size(n_icon_, icon_w, ui::line_height(Font::ICON));
      lv_obj_set_pos(n_icon_, pad_, (now_h - ui::line_height(Font::ICON)) / 2);
      lv_obj_remove_flag(n_icon_, LV_OBJ_FLAG_HIDDEN);
      const std::string t = degrees(now->temp);
      const lv_font_t *tf = fit(fonts_, t, left_w * 40 / 100, now_h * 70 / 100);
      const int tx = pad_ + icon_w + pad_;
      place(n_temp_, t, tf, tx, 0, now_h);
      const std::string wd = word(now->condition);
      const int wx = tx + text_w(t, tf) + pad_;
      if (!wd.empty() && left_w - wx > ui::px(40))
        place(n_word_, wd, fit(fonts_, wd, left_w - wx, now_h * 30 / 100), wx, 0, now_h);
      std::vector<std::pair<std::string, std::string>> facts;
      char buf[32];
      if (!std::isnan(now->feels)) facts.emplace_back(txt("feels"), degrees(now->feels));
      if (now->humidity >= 0) {
        snprintf(buf, sizeof buf, "%d%%", now->humidity);
        facts.emplace_back(txt("humidity"), buf);
      }
      if (!std::isnan(now->wind)) facts.emplace_back(txt("wind"), wind_text(now->wind, now->bearing));
      if (!std::isnan(now->uv)) {
        snprintf(buf, sizeof buf, "%d", static_cast<int>(std::lround(now->uv)));
        facts.emplace_back(txt("uv"), buf);
      }
      paint_facts(facts, left_w, ui::px(4), w_ - left_w - pad_, now_h - ui::px(8), std::max<int>(1, facts.size()));
      rule(rule1_, now_h - 1);
    }
    // A row per day.
    rows_n_ = std::min<int>(shown_.size(), ROWS);
    row_h_ = (h_ - rows_y_) / std::max(1, rows_n_);
    const int icon_w = ui::line_height(Font::ICON);
    for (int i = 0; i < rows_n_; ++i) {
      const Day &d = shown_[i];
      Row &r = rows_[i];
      const int y = rows_y_ + i * row_h_;
      place(r.day, day_name(d), fit(fonts_, "Wednesday", w_ * 24 / 100, row_h_ * 45 / 100), pad_, y, row_h_);
      ui::set_text(r.icon, ui::icon(icon_for(d.condition)));
      lv_obj_set_size(r.icon, icon_w, ui::line_height(Font::ICON));
      lv_obj_set_pos(r.icon, w_ * 27 / 100, y + (row_h_ - ui::line_height(Font::ICON)) / 2);
      lv_obj_remove_flag(r.icon, LV_OBJ_FLAG_HIDDEN);
      const std::string wd = word(d.condition);
      if (!wd.empty())
        place(r.word, wd, fit(fonts_, "Partly cloudy", w_ * 24 / 100, row_h_ * 35 / 100), w_ * 27 / 100 + icon_w + pad_, y, row_h_);
      // Rain when some is forecast, else the wind.
      std::string extra;
      if (d.chance > 0) {
        char buf[8];
        snprintf(buf, sizeof buf, "%d%%", d.chance);
        extra = buf;
      } else if (!std::isnan(d.rain) && d.rain > 0.004f) {
        extra = rain_text(d.rain);
      } else if (!std::isnan(d.wind)) {
        extra = wind_text(d.wind, d.bearing);
      }
      if (!extra.empty()) place(r.extra, extra, fit(fonts_, "NW 20 mph", w_ * 16 / 100, row_h_ * 35 / 100), w_ * 62 / 100, y, row_h_);
      const std::string temps = degrees(d.high) + " / " + degrees(d.low);
      place(r.temps, temps, fit(fonts_, "100\xC2\xB0 / 100\xC2\xB0", w_ * 20 / 100, row_h_ * 45 / 100), w_ - pad_, y, row_h_, true);
      if (i < rows_n_ - 1) {
        lv_obj_set_size(r.line, w_ - 2 * pad_, std::max(1, ui::px(1)));
        lv_obj_set_pos(r.line, pad_, y + row_h_ - 1);
        lv_obj_remove_flag(r.line, LV_OBJ_FLAG_HIDDEN);
      }
    }
  }

  void paint_detail(const Day &d) {
    const std::vector<const Hour *> hs = pick_hours(d.civil, HOURS);
    // The bands: the day and its word, the high and low, the forecast in words, the facts, and the hours when the
    // hourly forecast reaches the day.
    const bool with_hours = !hs.empty();
    const int b_day = h_ * (with_hours ? 13 : 14) / 100, b_temp = h_ * (with_hours ? 21 : 25) / 100;
    const int b_text = h_ * (with_hours ? 26 : 33) / 100;
    const int b_facts = with_hours ? h_ * 18 / 100 : h_ - b_day - b_temp - b_text;
    // The day at the left, the icon and word at the right of the same line.
    const std::string name = day_name(d);
    place(d_day_, name, fit(fonts_, name, w_ * 45 / 100, b_day * 60 / 100), pad_, 0, b_day);
    const std::string wd = word(d.condition);
    const int icon_w = ui::line_height(Font::ICON);
    const lv_font_t *wf = fit(fonts_, wd.empty() ? "X" : wd, w_ * 40 / 100, b_day * 45 / 100);
    const int ww = wd.empty() ? 0 : text_w(wd, wf);
    const int icon_x = w_ - pad_ - ww - pad_ / 2 - icon_w;
    ui::set_text(d_icon_, ui::icon(icon_for(d.condition)));
    lv_obj_set_size(d_icon_, icon_w, ui::line_height(Font::ICON));
    lv_obj_set_pos(d_icon_, icon_x, (b_day - ui::line_height(Font::ICON)) / 2);
    lv_obj_remove_flag(d_icon_, LV_OBJ_FLAG_HIDDEN);
    if (!wd.empty()) place(d_word_, wd, wf, w_ - pad_, 0, b_day, true);
    // The high large, the low beside it smaller.
    const std::string hi = degrees(d.high), lo = degrees(d.low);
    const lv_font_t *hf = fit(fonts_, hi, w_ * 40 / 100, b_temp * 80 / 100);
    const lv_font_t *lf = fit(fonts_, lo, w_ * 30 / 100, b_temp * 55 / 100);
    const int gap = pad_ * 2;
    const int total = text_w(hi, hf) + gap + text_w(lo, lf);
    const int x0 = std::max(pad_, (w_ - total) / 2);
    place(d_high_, hi, hf, x0, b_day, b_temp);
    place(d_low_, lo, lf, x0 + text_w(hi, hf) + gap, b_day, b_temp);
    // The forecast in words.
    place_wrapped(d_text_, sentence(d), fonts_, pad_, b_day + b_temp, w_ - 2 * pad_, b_text);
    // The facts.
    std::vector<std::pair<std::string, std::string>> facts;
    char buf[24];
    if (d.chance >= 0) {
      snprintf(buf, sizeof buf, "%d%%", d.chance);
      facts.emplace_back(txt("chance"), buf);
    }
    if (!std::isnan(d.rain)) facts.emplace_back(txt("rain"), rain_text(d.rain));
    if (!std::isnan(d.wind)) facts.emplace_back(txt("wind"), wind_text(d.wind, d.bearing));
    if (d.humidity >= 0) {
      snprintf(buf, sizeof buf, "%d%%", d.humidity);
      facts.emplace_back(txt("humidity"), buf);
    }
    if (!std::isnan(d.uv)) {
      snprintf(buf, sizeof buf, "%d", static_cast<int>(std::lround(d.uv)));
      facts.emplace_back(txt("uv"), buf);
    }
    const int fy = b_day + b_temp + b_text;
    rule(rule1_, fy);
    paint_facts(facts, pad_, fy + ui::px(4), w_ - 2 * pad_, b_facts - ui::px(8), std::min<int>(facts.size(), 5));
    if (!hs.empty()) {
      rule(rule2_, fy + b_facts);
      paint_hours(hs, fy + b_facts + ui::px(4), h_ - fy - b_facts - ui::px(8));
    }
  }

  BoldWeather *plugin_;
  int w_ = 0, h_ = 0, pad_ = 14, row_h_ = 0, rows_y_ = 0, rows_n_ = 0, detail_ = -1, hour_ = -1;
  int32_t today_ = 0;
  uint32_t epoch_ = 0;
  std::string entity_;
  std::vector<const lv_font_t *> fonts_;
  std::vector<Day> shown_;
  Fact facts_[FACTS];
  HourCol hours_[HOURS];
  Row rows_[ROWS];
  lv_obj_t *n_icon_{}, *n_word_{}, *n_temp_{}, *rule1_{}, *rule2_{};
  lv_obj_t *d_day_{}, *d_icon_{}, *d_word_{}, *d_high_{}, *d_low_{}, *d_text_{}, *note_{};
};

void BoldWeather::setup() {
  add_tile("forecast", [this]() { return new ForecastTile(this); });
  add_card("week", [this]() { return new WeekCard(this); }, true);
}

}  // namespace esphome::bold_weather
