"""Bold weather: the ESPHome side. smart_display.register_plugin() reads the id, version, tile memory and the screen's
texts from the manifest and translations/ beside this folder. The fonts named in plugin.yaml are handed to the C++ so
each tile can pick the largest that fits."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import font, smart_display
from esphome.const import CONF_ID

# The core component every Tessera screen has; the plugin API lives in it.
DEPENDENCIES = ["smart_display", "font"]

CONF_FONTS = "fonts"

bold_weather_ns = cg.esphome_ns.namespace("bold_weather")
BoldWeather = bold_weather_ns.class_("BoldWeather", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BoldWeather),
        cv.Optional(CONF_FONTS, default=[]): cv.ensure_list(cv.use_id(font.Font)),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    for font_id in config[CONF_FONTS]:
        cg.add(var.add_font(await cg.get_variable(font_id)))
    await smart_display.register_plugin(var, __file__)
