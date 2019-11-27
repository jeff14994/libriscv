#include "testable_instruction.hpp"
#include <cassert>
using namespace riscv;


void test_rv32c()
{
	const uint32_t memory = 65536;
	riscv::Machine<RISCV32> machine { {}, memory };
	//machine.verbose_instructions = true;

	// C.ANDI imm = [-32, 31] CI_CODE(0b100, 0b01):
	for (int i = 0; i < 64; i++)
	{
		rv32c_instruction ci;
		ci.CA.opcode  = 0b01;     // Quadrant 1
		ci.CA.funct6  = 0b100010; // ALU OP: ANDI
		ci.CAB.srd    = 0x2; // A0
		ci.CAB.imm04  = i & 31;
		ci.CAB.imm5   = i >> 5;

		const testable_insn<RISCV32> insn {
			.name  = "C.ANDI",
			.bits  = ci.whole,
			.reg   = RISCV::REG_ARG0,
			.index = i,
			.initial_value = 0xFFFFFFFF
		};
		bool b = validate<RISCV32>(machine, insn,
		[] (auto& cpu, const auto& insn) -> bool {
			if (insn.index < 32) {
				return cpu.reg(insn.reg) == (insn.initial_value & insn.index);
			}
			return cpu.reg(insn.reg) == (insn.initial_value & (insn.index-64));
		});
		assert(b);
	}

	printf("%lu instructions passed.\n", machine.cpu.registers().counter);
}