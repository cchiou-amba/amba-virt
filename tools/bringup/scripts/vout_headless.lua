--
-- tools/bringup/scripts/vout_headless.lua
--
-- Headless VOUT configuration: all controllers disabled.
-- For EVE-OS boards without physical display hardware.
--
-- Copyright (C) 2026, Ambarella International LLC
--

vout_0 = {
	status = "disable",
	type = "mipi_dsi",
	mode = "1080p",
}

vout_1 = {
	status = "disable",
	type = "mipi_dsi",
	mode = "1080p",
}

vout_2 = {
	status = "disable",
	type = "mipi_dsi",
	mode = "1080p",
}

_vout_config_ = {
	version = 1,
	vouts = {
		vout_0,
		vout_1,
		vout_2,
	},
}

return _vout_config_
