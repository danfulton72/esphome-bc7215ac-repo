"""BC7215A Universal A/C IR Controller - ESPHome external component.

The climate platform schema and to_code live in climate.py.
This file just declares the shared namespace + class reference.
"""
import esphome.codegen as cg
from esphome.components import climate

bc7215ac_ns = cg.esphome_ns.namespace("bc7215ac")
BC7215ACClimate = bc7215ac_ns.class_(
    "BC7215ACClimate",
    climate.Climate,
    cg.Component,
)