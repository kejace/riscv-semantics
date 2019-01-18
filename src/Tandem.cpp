#include <iostream>
#include <iomanip>
#include <sstream>

#include <string> 
#include <vector>

#include "simif.h"
#include "processor.h"
#include "encoding.h"
#include "devices.h"
#include "disasm.h"
#include "mmu.h"
#include "CircularBuffer.hpp"

#include "ElfFile.h"


typedef struct VerificationPacket {
  uint64_t pc;
  uint32_t instruction;
  bool exception;
  bool interrupt;
  uint8_t cause; // only 4 bits
  uint64_t addr;
  bool valid_addr;
  uint64_t data;
  bool valid_dst;
  uint8_t dst; // Bit for fpr/valid
} VerificationPacket;

class tandemspike_t : public simif_t {
public:
  tandemspike_t(const char* elf_path) : proc("rv64g", this, 0),outBuffer(40), disassembler(new disassembler_t(64)) {
    data_sz = 0x800000;
    data = (char*) calloc(data_sz, 1);
    ElfFile prog;
    prog.open(elf_path);
    for (auto& it : prog.getSections()) {
      if (it.base >= 0x80000000ull && ((((char*) it.base) + it.data_size) <= (char*)(0x80000000ull + data_sz))) {
	memcpy((void*) (data + (((uint64_t) it.base) - 0x80000000ull)), it.data, it.data_size);
      } else {
	std::cout << "Skipping section at " << it.base << std::endl;
      }
    }
    proc.get_state()->pc = 0x80000000;
  }

  char* addr_to_mem(reg_t addr) {return NULL;}
     // Does not have any real memory because we need to hijack every load and store.

  bool mmio_load(reg_t addr, size_t len, uint8_t* bytes){
    if ( addr > 0x80000000) {
      actual_packet.data = 0;
      memcpy(bytes, data+addr, len);
      memcpy(&(actual_packet.data), data+addr, len);
      actual_packet.addr = addr;
    }
    else {
      std::cout << "Want to read from less than 80m" << std::endl;
    }
  }

  bool mmio_store(reg_t addr, size_t len, const uint8_t* bytes){
    if ( addr > 0x80000000) {
      actual_packet.data = 0;
      memcpy(data+addr,bytes, len);
      memcpy(&(actual_packet.data), bytes, len);
      actual_packet.addr = addr;
    } else {
      std::cout << "Want to store from less than 80m" << std::endl;
    }
  }

  void proc_reset(unsigned id){}
  void dump_state() {
    std::cout << "pc = 0x" << std::hex << proc.get_state()->pc << std::endl;
    for (int i = 0 ; i < 32 ; i = i+1) {
        std::cout << "  x" << std::dec << i << " = 0x" << std::hex << proc.get_state()->XPR[i] << std::endl;
    }
  }

  void step(VerificationPacket synchronization_packet){
    actual_packet = synchronization_packet;

    if (synchronization_packet.interrupt) {
        // try to force the specified interrupt
        proc.get_state()->mip = 1 << synchronization_packet.cause;
    }

    actual_packet.pc = proc.get_state()->pc;
    // Read new instruction
    try {
        actual_packet.instruction = (uint32_t) proc.get_mmu()->access_icache(actual_packet.pc)->data.insn.bits();
    } catch(...) {
        // if the current instruction didn't cause an exception or an interrupt, we should have been able to get the current instruction
        if (!(synchronization_packet.interrupt || synchronization_packet.exception)) {
            std::cout << "[ERROR] access_icache(0x" << std::hex << actual_packet.pc <<  ") failed even though there was no interrupt or exception for the current verification packet." << std::endl;
        }
    }

    proc.step(1);

    if (synchronization_packet.interrupt && ((synchronization_packet.cause == 3) || (synchronization_packet.cause == 9) || (synchronization_packet.cause == 11))) {
        // if forcing MSIP, SEIP, or MEIP, zero out the MIP CSR
        proc.get_state()->mip = 0;
    }

    // check if instruction caused an exception, and if so, figure out the cause
    if (!synchronization_packet.interrupt) {
        if (proc.get_state()->pc == (proc.get_state()->mtvec & ~3)) {
            if (proc.get_state()->mcause >> 31) {
                actual_packet.interrupt = 1;
                actual_packet.exception = 0;
            } else {
                actual_packet.exception = 1;
                actual_packet.interrupt = 0;
            }
            actual_packet.cause = proc.get_state()->mcause;
        } else if (proc.get_state()->pc == (proc.get_state()->stvec & ~3)) {
            if (proc.get_state()->scause >> 31) {
                actual_packet.interrupt = 1;
                actual_packet.exception = 0;
            } else {
                actual_packet.exception = 1;
                actual_packet.interrupt = 0;
            }
            actual_packet.cause = proc.get_state()->scause;
        } else {
            actual_packet.exception = 0;
        }
    }

//    if (actual_packet.instruction != synchronization_packet.instruction) {
//        // instructions don't match, so we need to figure out the destination for the actual packet
//        // assume its always writing to the XPR specified in rd
//        actual_packet.dst = ((actual_packet.instruction >> 7) & 0x1F);
//        actual_packet.valid_dst = synchronization_packet.
//    }

    if (synchronization_packet.valid_dst) { // Packet valid
       actual_packet.data = proc.get_state()->XPR[actual_packet.dst & 0x1F];
    }


    // Move forward the processor and adjust if required.
  }


  bool comparePackets(VerificationPacket procP, VerificationPacket spikeP) {
    bool match = true;
    match = match && (procP.pc == spikeP.pc);
    match = match && (procP.instruction == spikeP.instruction);
    match = match && (procP.exception == spikeP.exception);
    match = match && (procP.interrupt == spikeP.interrupt);
    if (procP.exception || procP.interrupt) {
        match = match && (procP.cause == spikeP.cause);
    } else {
        match = match && (procP.valid_dst || procP.valid_addr ) &&(procP.data == spikeP.data);
// Ajouter une hypothese sur la validité
        match = match && (procP.valid_dst) && (procP.dst == spikeP.dst);
        match = match && (procP.valid_addr) && (procP.addr == spikeP.addr);
    }
    return match;
  }


  std::string verificationPacketToString(VerificationPacket p) {
    std::ostringstream buffer;
    buffer << "[inst 0x" << std::setfill('0') << std::setw(16) << std::hex << packets << "] ";
    // pc
    buffer << "0x" << std::setfill('0') << std::setw(16) << std::hex << p.pc << ": ";
    // instruction data
    buffer << "(0x" << std::setfill('0') << std::setw(8) << std::hex << p.instruction << ") ";
    // instruction disassembled
    std::string assembly = (disassembler->disassemble(p.instruction));
    buffer << std::left << std::setfill(' ') << std::setw(32) << assembly;
    if (p.exception) {
      switch (p.cause) {
      case 0:
	buffer << " [Exception: Instruction address misaligned]";
	break;
      case 1:
	buffer << " [Exception: Instruction access fault]";
	break;
      case 2:
	buffer << " [Exception: Illegal instruction]";
	break;
      case 3:
	buffer << " [Exception: Breakpoint]";
	break;
      case 4:
	buffer << " [Exception: Load address misaligned]";
	break;
      case 5:
	buffer << " [Exception: Load access fault]";
	break;
      case 6:
	buffer << " [Exception: Store/AMO address misaligned]";
	break;
      case 7:
	buffer << " [Exception: Store/AMO access fault]";
	break;
      case 8:
	buffer << " [Exception: Environment call from U-mode]";
	break;
      case 9:
	buffer << " [Exception: Environment call from S-mode]";
	break;
      case 10:
	buffer << " [Exception: Environment call from H-mode]";
	break;
      case 11:
	buffer << " [Exception: Environment call from M-mode]";
	break;
      case 12:
	buffer << " [Exception: Instruction page fault]";
	break;
      case 13:
	buffer << " [Exception: Load page fault]";
	break;
      case 15:
	buffer << " [Exception: Store/AMO page fault]";
	break;
      default:
	buffer << " [Unknown Exception]";
      }
    } else if (p.interrupt) {
      switch (p.cause) {
      case 0x00:
	buffer << " [Interrupt: User software interrupt]";
	break;
      case 0x01:
	buffer << " [Interrupt: Supervisor software interrupt]";
	break;
      case 0x02:
	buffer << " [Interrupt: Hypervisor software interrupt]";
	break;
      case 0x03:
	buffer << " [Interrupt: Machine software interrupt]";
	break;
      case 0x04:
	buffer << " [Interrupt: User timer interrupt]";
	break;
      case 0x05:
	buffer << " [Interrupt: Supervisor timer interrupt]";
	break;
      case 0x06:
	buffer << " [Interrupt: Hypervisor timer interrupt]";
	break;
      case 0x07:
	buffer << " [Interrupt: Machine timer interrupt]";
	break;
      case 0x08:
	buffer << " [Interrupt: User external interrupt]";
	break;
      case 0x09:
	buffer << " [Interrupt: Supervisor external interrupt]";
	break;
      case 0x0A:
	buffer << " [Interrupt: Hypervisor external interrupt]";
	break;
      case 0x0B:
	buffer << " [Interrupt: Machine external interrupt]";
	break;
      default:
	buffer << " [Unknown Interrupt]";
      }
    } else if (p.dst & 0x40) {
      // destination register
      const char* regName = NULL;
      if (p.dst & 0x20) {
	regName = fpr_name[p.dst & 0x1f];
      } else {
	regName = xpr_name[p.dst & 0x1f];
      }
      buffer << " [" << regName << " = 0x" << std::hex << p.data << "]";
    }
    switch (p.instruction & 0x7F) {
    case 0x03: // Load
    case 0x23: // Store
    case 0x2F: // AMO
    case 0x07: // FP-Load
    case 0x27: // FP-Store
      buffer << " (addr = 0x" << std::hex << p.addr << ")";
    }
    return buffer.str();
  }


  bool check_packet(VerificationPacket packet){
    // Step
    if (packet.interrupt || packet.exception) {
        consecutive_traps += 1;
    } else {
        consecutive_traps = 0;
    }

    packets++;
    bool match = comparePackets(packet, actual_packet);

    if (!match) {
        errors++;
        std::ostringstream buffer;
        buffer << "[ERROR] Verification error in packet " << packets << " (instruction ANDY screwed up here)" << std::endl;
        buffer << "  [PROC]  " << verificationPacketToString(packet) << std::endl;
        buffer << "  [SPIKE] " << verificationPacketToString(actual_packet);
        outBuffer.addLine(buffer.str());
        outBuffer.printToOStream(&std::cerr, 20);
    } else {
        outBuffer.addLine(verificationPacketToString(packet));
    }

    // XXX: this was to temporarily print everything
    // outBuffer.printToOStream(&std::cerr, 20000);

    if ((errors > 40) || (consecutive_traps > 2)) {
        if (consecutive_traps > 2) {
            std::ostringstream buffer;
            buffer << "[ERROR] More than 2 consecutive traps" << std::endl;
            outBuffer.addLine(buffer.str());
        }
        outBuffer.printToOStream(&std::cerr, 20);
        // and let's abort!
        exit(1);
    }

    return match;
  }

private:
  disassembler_t *disassembler;
  processor_t proc;
  unsigned int packets;
  unsigned int consecutive_traps;
  unsigned int errors;
  char* data; // Memory of the machine
  VerificationPacket actual_packet;
  size_t data_sz;
  CircularBuffer outBuffer;

};

int main(int argc, char* argv[]) {
  const char* elf_path = "../test/build/mandelbrot64";
  // TODO Add some ways to read the path from the command line
  tandemspike_t sim(elf_path);
  VerificationPacket haskell_packet;
  while (1) {
    // step the haskell model
    int read;
    std::string command;
    std::cout << "n";
    do {
      std::cin >> command;
    } while (command != "s");
    // Parse packet
    std::cin >> haskell_packet.pc;
    std::cin >> haskell_packet.instruction;
    std::cin >> haskell_packet.exception;
    std::cin >> haskell_packet.interrupt;
    std::cin >> haskell_packet.cause;
    std::cin >> haskell_packet.addr;
    std::cin >> haskell_packet.valid_addr;
    std::cin >> haskell_packet.data;
    std::cin >> haskell_packet.valid_dst;
    std::cin >> haskell_packet.dst;
    do {
      std::cin >> command;
    } while (command != "e");  
    // parse the verification packet
    // step from this class
    // Check_packet
  }
}