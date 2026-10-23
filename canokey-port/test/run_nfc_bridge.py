"""Run the freestanding RISC-V bridge harness without target hardware."""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).parent / "build" / "deps"))

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV32
from unicorn.riscv_const import UC_RISCV_REG_A0, UC_RISCV_REG_PC, UC_RISCV_REG_RA, UC_RISCV_REG_SP


def main():
    emulator = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
    with open(sys.argv[1], "rb") as source:
        elf = ELFFile(source)
        load_segments = [segment for segment in elf.iter_segments() if segment["p_type"] == "PT_LOAD"]
        page_size = 0x1000
        load_start = min(segment["p_vaddr"] for segment in load_segments) & ~(page_size - 1)
        load_end = max(segment["p_vaddr"] + segment["p_memsz"] for segment in load_segments)
        load_end = (load_end + page_size - 1) & ~(page_size - 1)
        emulator.mem_map(load_start, load_end - load_start)
        for segment in load_segments:
            emulator.mem_write(segment["p_vaddr"], segment.data())
        entry = elf.header["e_entry"]
    emulator.mem_map(0x200000, 0x10000)
    emulator.mem_map(0x300000, 0x1000)
    emulator.reg_write(UC_RISCV_REG_SP, 0x210000 - 16)
    emulator.reg_write(UC_RISCV_REG_RA, 0x300000)
    emulator.emu_start(entry, 0x300000, timeout=10000000, count=5000000)
    if emulator.reg_read(UC_RISCV_REG_PC) != 0x300000:
        raise RuntimeError("NFC bridge test did not return within the execution budget")
    result = emulator.reg_read(UC_RISCV_REG_A0)
    print("NFC bridge tests passed" if result == 0 else f"NFC bridge test failed at line {result}")
    return bool(result)


if __name__ == "__main__":
    sys.exit(main())
