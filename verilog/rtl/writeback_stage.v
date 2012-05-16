//
// CPU pipeline writeback stage
//  - Handle aligning memory reads that are smaller than a word
//  - Determine what the source of the register writeback should be
//  - Control signals to control commit of values back to the register file
//

module writeback_stage(
	input					clk,
	input [31:0]			ma_instruction,
	input [31:0]			ma_pc,
	input [1:0]				ma_strand,
	input [6:0]				ma_writeback_reg,
	input					ma_writeback_is_vector,	
	input	 				ma_has_writeback,
	input [15:0]			ma_mask,
	input 					dcache_hit,
	output reg				wb_writeback_is_vector = 0,	
	output reg				wb_has_writeback = 0,
	output reg[6:0]			wb_writeback_reg = 0,
	output reg[511:0]		wb_writeback_value = 0,
	output reg[15:0]		wb_writeback_mask = 0,
	input 					ma_was_access,
	input [511:0]			data_from_dcache,
	input					dcache_load_collision,
	input 					stbuf_rollback,
	input [511:0]			ma_result,
	input [3:0]				ma_reg_lane_select,
	input [3:0]				ma_cache_lane_select,
	output reg				wb_rollback_request = 0,
	output reg[31:0]		wb_rollback_pc = 0,
	output 					wb_suspend_request);

	reg[511:0]				writeback_value_nxt = 0;
	reg[15:0]				mask_nxt = 0;
	reg[31:0]				aligned_read_value = 0;
	reg[15:0]				half_aligned = 0;
	reg[7:0]				byte_aligned = 0;
	wire[31:0]				lane_value;

	wire is_fmt_c = ma_instruction[31:30] == 2'b10;
	wire is_load = is_fmt_c && ma_instruction[29];
	wire[3:0] c_op_type = ma_instruction[28:25];
	wire is_control_register_transfer = is_fmt_c
		&& c_op_type == 4'b0110;
	wire cache_miss = ~dcache_hit && ma_was_access && is_load && !dcache_load_collision;

	always @*
	begin
		if (dcache_load_collision)
		begin
			// Data came in one cycle too late.  Roll back and retry.
			wb_rollback_pc = ma_pc - 4;
			wb_rollback_request = 1;
		end
		else if (cache_miss || stbuf_rollback)
		begin
			// Data cache read miss or store buffer rollback (full or synchronized store)
			wb_rollback_pc = ma_pc - 4;
			wb_rollback_request = 1;
		end
		else if (ma_has_writeback && !ma_writeback_is_vector
			&& ma_writeback_reg[4:0] == 31 && is_load)
		begin
			// A load has occurred to PC, branch to that address
			// Note that we checked for a cache miss *before* we checked
			// this case, otherwise we'd just jump to address zero.
			wb_rollback_pc = aligned_read_value;
			wb_rollback_request = 1;
		end
		else
		begin
			wb_rollback_pc = 0;
			wb_rollback_request = 0;
		end
	end
	
	assign wb_suspend_request = cache_miss || stbuf_rollback;

	lane_select_mux lsm(
		.value_i(data_from_dcache),
		.value_o(lane_value),
		.lane_select_i(ma_cache_lane_select));
	
	wire[511:0] endian_twiddled_data = {
		data_from_dcache[487:480], data_from_dcache[495:488], data_from_dcache[503:496], data_from_dcache[511:504], 
		data_from_dcache[455:448], data_from_dcache[463:456], data_from_dcache[471:464], data_from_dcache[479:472], 
		data_from_dcache[423:416], data_from_dcache[431:424], data_from_dcache[439:432], data_from_dcache[447:440], 
		data_from_dcache[391:384], data_from_dcache[399:392], data_from_dcache[407:400], data_from_dcache[415:408], 
		data_from_dcache[359:352], data_from_dcache[367:360], data_from_dcache[375:368], data_from_dcache[383:376], 
		data_from_dcache[327:320], data_from_dcache[335:328], data_from_dcache[343:336], data_from_dcache[351:344], 
		data_from_dcache[295:288], data_from_dcache[303:296], data_from_dcache[311:304], data_from_dcache[319:312], 
		data_from_dcache[263:256], data_from_dcache[271:264], data_from_dcache[279:272], data_from_dcache[287:280], 
		data_from_dcache[231:224], data_from_dcache[239:232], data_from_dcache[247:240], data_from_dcache[255:248], 
		data_from_dcache[199:192], data_from_dcache[207:200], data_from_dcache[215:208], data_from_dcache[223:216], 
		data_from_dcache[167:160], data_from_dcache[175:168], data_from_dcache[183:176], data_from_dcache[191:184], 
		data_from_dcache[135:128], data_from_dcache[143:136], data_from_dcache[151:144], data_from_dcache[159:152], 
		data_from_dcache[103:96], data_from_dcache[111:104], data_from_dcache[119:112], data_from_dcache[127:120], 
		data_from_dcache[71:64], data_from_dcache[79:72], data_from_dcache[87:80], data_from_dcache[95:88], 
		data_from_dcache[39:32], data_from_dcache[47:40], data_from_dcache[55:48], data_from_dcache[63:56], 
		data_from_dcache[7:0], data_from_dcache[15:8], data_from_dcache[23:16], data_from_dcache[31:24] 	
	};

	// Byte aligner.  ma_result still contains the effective address,
	// so use that to determine where the data will appear.
	always @*
	begin
		case (ma_result[1:0])
			2'b00: byte_aligned = lane_value[31:24];
			2'b01: byte_aligned = lane_value[23:16];
			2'b10: byte_aligned = lane_value[15:8];
			2'b11: byte_aligned = lane_value[7:0];
		endcase
	end

	// Halfword aligner.  Same as above.
	always @*
	begin
		case (ma_result[1])
			1'b0: half_aligned = { lane_value[23:16], lane_value[31:24] };
			1'b1: half_aligned = { lane_value[7:0], lane_value[15:8] };
		endcase
	end

	// Pick the proper aligned result and sign extend as requested.
	always @*
	begin
		case (c_op_type)		// Load width
			// Unsigned byte
			`MEM_B: aligned_read_value = { 24'b0, byte_aligned };	

			// Signed byte
			`MEM_BX: aligned_read_value = { {24{byte_aligned[7]}}, byte_aligned }; 

			// Unsigned half-word
			`MEM_S: aligned_read_value = { 16'b0, half_aligned };

			// Signed half-word
			`MEM_SX: aligned_read_value = { {16{half_aligned[15]}}, half_aligned };

			// Word (100) and others
			default: aligned_read_value = { lane_value[7:0], lane_value[15:8],
				lane_value[23:16], lane_value[31:24] };	
		endcase
	end

	always @*
	begin
		if (ma_instruction[31:25] == 7'b1000101)
		begin
			// Synchronized store.  Success value comes back from cache
			writeback_value_nxt = data_from_dcache;
			mask_nxt = 16'hffff;
		end
		else if (is_load && !is_control_register_transfer)
		begin
			// Load result
			if (c_op_type[3] == 0 && c_op_type != `MEM_BLOCK)
			begin
				writeback_value_nxt = {16{aligned_read_value}}; // Scalar Load
				mask_nxt = 16'hffff;
			end
			else
			begin
				if (c_op_type == `MEM_BLOCK || c_op_type == `MEM_BLOCK_M
					|| c_op_type == `MEM_BLOCK_IM)
				begin
					// Block load
					mask_nxt = ma_mask;	
					writeback_value_nxt = endian_twiddled_data;	// Vector Load
				end
				else 
				begin
					// Strided or gather load
					// Grab the appropriate lane.
					writeback_value_nxt = {16{aligned_read_value}};
					mask_nxt = (16'h8000 >> ma_reg_lane_select) & ma_mask;	// sg or strided
				end
			end
		end
		else
		begin
			// Arithmetic expression
			writeback_value_nxt = ma_result;
			mask_nxt = ma_mask;
		end
	end

	wire do_writeback = ma_has_writeback && !wb_rollback_request;

	always @(posedge clk)
	begin
		wb_writeback_value 			<= #1 writeback_value_nxt;
		wb_writeback_mask 						<= #1 mask_nxt;
		wb_writeback_is_vector 		<= #1 ma_writeback_is_vector;
		wb_has_writeback 			<= #1 do_writeback;
		wb_writeback_reg 			<= #1 ma_writeback_reg;
	end

`ifdef ENABLE_REG_DISPLAY
	//
	// Debug display
	//
	integer _display_lane;
	reg[31:0] _lane_value;
	
	always @(posedge clk)
	begin
		if (do_writeback)
		begin
			if (ma_writeback_is_vector)
			begin
				$write("%08x [st %d] v%d{%b} <= ", ma_pc - 4, ma_writeback_reg[6:5], 
					ma_writeback_reg[4:0], mask_nxt);
				for (_display_lane = 15; _display_lane >= 0; _display_lane = _display_lane - 1)
				begin
					_lane_value = writeback_value_nxt >> (32 * _display_lane);
					$write("%08x ", _lane_value);
				end
				$display("");
			end
			else
			begin
				$display("%08x [st %d] s%d = %08x", ma_pc - 4, ma_writeback_reg[6:5], 
					ma_writeback_reg[4:0], writeback_value_nxt[31:0]);
			end
		end
	end
`endif
endmodule
