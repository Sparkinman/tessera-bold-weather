# Bold weather

A weather tile in large bold letters for a screen you read from across the room, with a week card behind it.

- **Bold forecast** (tile): the weather now (its icon and word, and the temperature large) and the coming days, as
  many columns as fit: the day, its icon, the high large and the low smaller. On a tall tile the days sit under the
  weather now; on a short, wide one they sit at its right.
- A tap opens **Bold week**: a strip with the weather now (temperature, condition, feels like, humidity, wind with
  its direction, UV) above a large row per day (the day, icon and word, rain or wind, high and low).
- A tap on a day shows it in full: the high and low large, a short forecast in words ("Partly cloudy, a high of 36°
  and a low of 24°. Light winds from the east, around 6 mph. No rain expected."), the rain, wind, humidity and UV,
  and, for today and tomorrow, the coming hours. A tap there or Back goes back to the week.

The forecast comes from Home Assistant's `weather.get_forecasts`, daily and then hourly, asked for the tile's weather
entity when it shows and about every half hour. The forecast in words is put together on the screen from the
numbers, so it works with any weather integration (met.no gives no chance of rain or written forecast of its own).
The hourly answer is cut to what fits a plugin message, about the next fifteen hours.

## Which screens

Made for the **larger touchscreens** (7" and 10" ESP32-P4 boards with 16 MB of flash). Tessera's own fonts stop at a
fixed size, so this plugin brings bold Roboto (the screen's own typeface) in four sizes, about 450 KB of flash, which
the 4 MB boards cannot spare.

## Install for testing

Copy this folder to `/config/tessera-plugins/bold_weather/` on Home Assistant. It then shows on Tessera's Plugins page
labelled Test. Add it to a screen and place the **Bold forecast** tile from the library's Plugins group.
