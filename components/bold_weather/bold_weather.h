#pragma once
#include <cmath>
#include <string>
#include <vector>
#include "esphome/core/component.h"
#include "esphome/components/font/font.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::bold_weather {

// One day of Home Assistant's daily forecast.
struct Day {
  int32_t civil = 0;  // days since 1970-01-01 of its date
  std::string condition;
  float high = NAN, low = NAN, rain = NAN, wind = NAN, bearing = NAN, uv = NAN;
  int chance = -1, humidity = -1;
};

// One hour of the hourly forecast.
struct Hour {
  uint32_t epoch = 0;  // its start, seconds since 1970 (UTC)
  std::string condition;
  float temp = NAN, rain = NAN;
};

// The plugin: a forecast tile and its week card. It asks Home Assistant for each weather entity's daily forecast
// (weather.get_forecasts) and keeps the answer, so the tile and the card share one ask.
class BoldWeather : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void on_message(JsonObjectConst message) override;

  void add_font(font::Font *f) { fonts_.push_back(f); }
  const std::vector<font::Font *> &fonts() const { return fonts_; }

  // A tile or card that wants to hear when a forecast came.
  struct Listener {
    virtual ~Listener() = default;
    virtual void on_forecast(const std::string &entity) = 0;
  };
  void listen(Listener *l) { listeners_.push_back(l); }
  void forget(Listener *l);
  // Asks for the entity's forecast unless one was asked in the last ten minutes (`force` asks anyway after a minute).
  void ask(const std::string &entity, bool force = false);
  const std::vector<Day> *days(const std::string &entity) const;
  // Units the tile read from the entity's attributes, for the card.
  void set_units(const std::string &entity, const std::string &wind, const std::string &rain);
  std::string wind_unit(const std::string &entity) const;
  std::string rain_unit(const std::string &entity) const;
  const std::vector<Hour> *hours(const std::string &entity) const;
  // The weather now, from the entity's state and attributes (the tile and card hand them over).
  struct Now {
    std::string condition, pressure_unit;
    float temp = NAN, feels = NAN, wind = NAN, bearing = NAN, pressure = NAN, dew = NAN, uv = NAN;
    int humidity = -1, clouds = -1;
  };
  void set_now(const std::string &entity, JsonObjectConst data);
  const Now *now(const std::string &entity) const;

 protected:
  struct Entry {
    std::string entity, wind_unit, rain_unit;
    std::vector<Day> days;
    std::vector<Hour> hours;
    Now now;
    uint32_t asked_s = 0, re = 0, hours_re = 0;
    bool have = false, have_now = false;
  };
  Entry &entry(const std::string &entity);
  const Entry *find(const std::string &entity) const;
  std::vector<font::Font *> fonts_;
  std::vector<Entry> entries_;
  std::vector<Listener *> listeners_;
};

}  // namespace esphome::bold_weather
