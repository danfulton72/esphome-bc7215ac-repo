"""Climate platform for the bc7215ac external component.

ESPHome resolves 'platform: bc7215ac' inside a 'climate:' block by
importing esphome.components.climate.bc7215ac, which maps to this file
(components/bc7215ac/climate.py) when the external_component path is set.
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, uart, switch, binary_sensor
from esphome.const import CONF_ID
from . import bc7215ac_ns, BC7215ACClimate

AUTO_LOAD = ["climate"]
DEPENDENCIES = ["uart"]

CONF_UART_ID  = "uart_id"
CONF_MOD_PIN  = "mod_pin"
CONF_BUSY_PIN = "busy_pin"

CONFIG_SCHEMA = climate.climate_schema(BC7215ACClimate).extend(
    {
        cv.Required(CONF_UART_ID): cv.use_id(uart.UARTComponent),
        cv.Optional(CONF_MOD_PIN):  cv.use_id(switch.Switch),
        cv.Optional(CONF_BUSY_PIN): cv.use_id(binary_sensor.BinarySensor),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)

    uart_var = await cg.get_variable(config[CONF_UART_ID])
    cg.add(var.set_uart(uart_var))

    if CONF_MOD_PIN in config:
        mod_var = await cg.get_variable(config[CONF_MOD_PIN])
        cg.add(var.set_mod_pin(mod_var))

    if CONF_BUSY_PIN in config:
        busy_var = await cg.get_variable(config[CONF_BUSY_PIN])
        cg.add(var.set_busy_pin(busy_var))