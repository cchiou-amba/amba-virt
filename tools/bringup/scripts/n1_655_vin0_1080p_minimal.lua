--
-- tools/bringup/scripts/n1_655_vin0_1080p_minimal.lua
--
-- Copyright (C) 2026, Ambarella International LLC
--

vsrc_0 = {
	vsrc_id = 0,
	mode = "1080p",
	hdr_mode = "linear",
	fps = 30,
	bits = 0,
}

chan_0 = {
	id = 0,
	vsrc = vsrc_0,
	vsrc_ctx = 0,
	img_stats_src_chan = "chan_0",
	sensor_ctrl = 1,
	max_padding_width = 0,
	idsp_fps = 0,
	lens_warp = 0,
	max_main_input_width = 0,
	mctf_cmpr = 0,
	c2y_burst_tile = 0,
	extra_downscale = 0,
	high_perf_enable = 1,
	main = {
		max_output = {0, 0},
		input      = {0, 0, 0, 0},
		output     = {0, 0, 1920, 1080},
	},
	second = {
		max_output = {0, 0},
		input      = {0, 0, 0, 0},
		output     = {0, 0, 0, 0},
	},
	third = {
		max_output = {0, 0},
		input      = {0, 0, 0, 0},
		output     = {0, 0, 0, 0},
	},
	fourth = {
		max_output = {0, 0},
		input      = {0, 0, 0, 0},
		output     = {0, 0, 0, 0},
	},
	fifth = {
		max_output = {0, 0},
		input      = {0, 0, 0, 0},
		output     = {0, 0, 0, 0},
	},
	pyramid = {
		input_buf_id = 0,
		scale_type = 0,
		buf_addr = 0x0,
		buf_size = 0x0,
		manual_feed = 0,
		item_num = 0,
		layer_map = 0x0,
		layers = {
			{
				crop_win = {0, 0, 0, 0},
			},
			{
				crop_win = {0, 0, 0, 0},
			},
			{
				crop_win = {0, 0, 0, 0},
			},
			{
				crop_win = {0, 0, 0, 0},
			},
			{
				crop_win = {0, 0, 0, 0},
			},
			{
				crop_win = {0, 0, 0, 0},
			},
			{
				crop_win = {0, 0, 0, 0},
			},
		},
	},
}

stream_0 = {
	id = 0,
	max_size = {1920, 1080},
	max_M = 1,
	fast_seek_enable = 0,
	two_ref_enable = 0,
	max_svct_layers_minus_1 = 0,
	max_num_minus_1_ltrs = 0,
	codec_enable = 2,
}

_resource_config_ = {
	version = 1,
	log_level = 0,
	channels = {
		chan_0,
	},
	canvas = {
		{
			type = "encode",
			size = {0, 0},
			source = {"chan_0.main",},
			extra_dram_buf = 0,
		},
	},
	streams = {
		stream_0,
	},
}

return _resource_config_
