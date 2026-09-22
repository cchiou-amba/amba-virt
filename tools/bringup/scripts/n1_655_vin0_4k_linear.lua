--
-- tools/bringup/scripts/n1_655_vin0_4k_linear.lua
--
-- Copyright (C) 2026, Ambarella International LLC
--

vsrc_0 = {
	vsrc_id = 0,
	mode = "3840x2160",
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
	mctf_cmpr = 1,
	c2y_burst_tile = 1,
	extra_downscale = 0,
	high_perf_enable = 1,
	main = {
		max_output = {0, 0},
		input      = {0, 0, 0, 0},
		output     = {0, 0, 3840, 2160},
	},
	second = {
		max_output = {0, 0},
		input      = {0, 0, 0, 0},
		output     = {0, 0, 720, 480},
	},
	third = {
		max_output = {0, 0},
		input      = {0, 0, 3840, 2160},
		output     = {0, 0, 1920, 1080},
	},
	fourth = {
		max_output = {0, 0},
		input      = {0, 0, 3840, 2160},
		output     = {0, 0, 1920, 1080},
	},
	fifth = {
		max_output = {0, 0},
		input      = {0, 0, 0, 0},
		output     = {0, 0, 1280, 720},
	},
	pyramid = {
		input_buf_id = 4,
		scale_type = 0,
		buf_addr = 0x0,
		buf_size = 0x0,
		manual_feed = 0,
		item_num = 0,
		layer_map = 0x7f,
		layers = {
			{ crop_win = {0, 0, 0, 0} },
			{ crop_win = {0, 0, 0, 0} },
			{ crop_win = {0, 0, 0, 0} },
			{ crop_win = {0, 0, 0, 0} },
			{ crop_win = {0, 0, 0, 0} },
			{ crop_win = {0, 0, 0, 0} },
			{ crop_win = {0, 0, 0, 0} },
		},
	},
}

stream_0 = {
	id = 0,
	max_size = {3840, 2160},
	max_M = 1,
	fast_seek_enable = 0,
	two_ref_enable = 0,
	max_svct_layers_minus_1 = 0,
	max_num_minus_1_ltrs = 0,
	codec_enable = 2,
}

stream_1 = {
	id = 1,
	max_size = {1920, 1080},
	max_M = 1,
	fast_seek_enable = 0,
	two_ref_enable = 0,
	max_svct_layers_minus_1 = 0,
	max_num_minus_1_ltrs = 0,
	codec_enable = 2,
}

stream_2 = {
	id = 2,
	max_size = {720, 480},
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
	channels = { chan_0 },
	canvas = {
		{
			type = "encode",
			size = {0, 0},
			source = {"chan_0.main"},
			extra_dram_buf = 0,
		},
		{
			type = "encode",
			size = {0, 0},
			source = {"chan_0.second"},
			extra_dram_buf = 0,
		},
		{
			type = "encode",
			size = {0, 0},
			source = {"chan_0.third"},
			extra_dram_buf = 0,
		},
		{
			type = "encode",
			size = {0, 0},
			source = {"chan_0.fourth"},
			extra_dram_buf = 0,
		},
	},
	streams = { stream_0, stream_1, stream_2 },
}

return _resource_config_
