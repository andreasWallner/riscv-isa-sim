// See LICENSE for license details.

#include "sim.h"
#include "decode.h"
#include "disasm.h"
#include "mmu.h"
#include <sys/mman.h>
#include <termios.h>
#include <map>
#include <iostream>
#include <iomanip>
#include <climits>
#include <cinttypes>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <math.h>

using std::ostream;
using std::cerr;
using std::hex;
using std::dec;
using std::setfill;
using std::left;
using std::right;
using std::setw;
using std::endl;

#define STR_(X) #X      // these definitions allow to use a macro as a string
#define STR(X) STR_(X)

DECLARE_TRAP(-1, interactive)

processor_t *sim_t::get_core(const std::string& i)
{
  char *ptr;
  unsigned long p = strtoul(i.c_str(), &ptr, 10);
  if (*ptr || p >= procs.size())
    throw trap_interactive();
  return get_core(p);
}

static std::string readline(int fd)
{
  struct termios tios;
  bool noncanonical = tcgetattr(fd, &tios) == 0 && (tios.c_lflag & ICANON) == 0;

  std::string s;
  for (char ch; read(fd, &ch, 1) == 1; )
  {
    if (ch == '\x7f')
    {
      if (s.empty())
        continue;
      s.erase(s.end()-1);

      if (noncanonical && write(fd, "\b \b", 3) != 3)
        ; // shut up gcc
    }
    else if (noncanonical && write(fd, &ch, 1) != 1)
      ; // shut up gcc

    if (ch == '\n')
      break;
    if (ch != '\x7f')
      s += ch;
  }
  return s;
}

#ifdef HAVE_BOOST_ASIO
// read input command string
std::string sim_t::rin(streambuf *bout_ptr) {
  std::string s;
  if (acceptor_ptr) { // if we are listening, get commands from socket
    try {
      socket_ptr = new tcp::socket(*io_service_ptr);
      acceptor_ptr->accept(*socket_ptr); // wait for someone to open connection
      boost::asio::streambuf buf;
      boost::asio::read_until(*socket_ptr, buf, "\n"); // wait for command
      s = buffer_cast<const char*>(buf.data());
      erase_all(s, "\r");  // get rid off any cr and lf
      erase_all(s, "\n");
      // The socket client is a web server and it appends the IP of the computer
      // that sent the command from its web browser.

      // For now, erase the IP if it is there.
      regex re(" ((25[0-5]|2[0-4][0-9]|[01]?[0-9]?[0-9])\\.){3}"
               "(25[0-5]|2[0-4][0-9]|[01]?[0-9]?[0-9])$");
      s = regex_replace(s, re, (std::string)"");

      // TODO: check the IP against the IP used to upload RISC-V source files
    } catch (std::exception& e) {
      cerr << e.what() << endl;
    }
    // output goes to socket
    sout.rdbuf(bout_ptr);
  } else { // if we are not listening on a socket, get commands from terminal
    cerr << ": " << std::flush;
    s = readline(2); // 2 is stderr, but when doing reads it reverts to stdin
    // output goes to stderr
    sout.rdbuf(cerr.rdbuf());
  }
  return s;
}

// write sout to socket (via bout)
void sim_t::wout(streambuf *bout_ptr) {
  if (acceptor_ptr) { // only if a socket has been created
    try {
      boost::system::error_code ignored_error;
      boost::asio::write(*socket_ptr, *bout_ptr, transfer_all(), ignored_error);
      socket_ptr->close(); // close the socket after each command input/ouput
                           //  This is need to in order to make the socket interface
                           //  acessible by HTTP GET via a socket client in a web server.
    } catch (std::exception& e) {
      cerr << e.what() << endl;
    }
  }
}
#endif

void sim_t::interactive()
{
  typedef void (sim_t::*interactive_func)(const std::string&, const std::vector<std::string>&);
  std::map<std::string,interactive_func> funcs;

  funcs["run"] = &sim_t::interactive_run_noisy;
  funcs["r"] = funcs["run"];
  funcs["rs"] = &sim_t::interactive_run_silent;
  funcs["vreg"] = &sim_t::interactive_vreg;
  funcs["reg"] = &sim_t::interactive_reg;
  funcs["freg"] = &sim_t::interactive_freg;
  funcs["fregh"] = &sim_t::interactive_fregh;
  funcs["fregs"] = &sim_t::interactive_fregs;
  funcs["fregd"] = &sim_t::interactive_fregd;
  funcs["pc"] = &sim_t::interactive_pc;
  funcs["mem"] = &sim_t::interactive_mem;
  funcs["str"] = &sim_t::interactive_str;
  funcs["until"] = &sim_t::interactive_until_silent;
  funcs["untiln"] = &sim_t::interactive_until_noisy;
  funcs["while"] = &sim_t::interactive_until_silent;
  funcs["quit"] = &sim_t::interactive_quit;
  funcs["q"] = funcs["quit"];
  funcs["help"] = &sim_t::interactive_help;
  funcs["h"] = funcs["help"];

  while (!done())
  {
    std::string s;
#ifdef HAVE_BOOST_ASIO
    streambuf bout; // socket output
    s = rin(&bout); // get command string from socket or terminal
#else
    cerr << ": " << std::flush;
    s = readline(2); // 2 is stderr, but when doing reads it reverts to stdin
#endif

    std::stringstream ss(s);
    std::string cmd, tmp;
    std::vector<std::string> args;

    if (!(ss >> cmd))
    {
      set_procs_debug(true);
      step(1);
#ifdef HAVE_BOOST_ASIO
      wout(&bout); // socket output, if required
#endif
      continue;
    }

    while (ss >> tmp)
      args.push_back(tmp);

    try
    {
      if (funcs.count(cmd))
        (this->*funcs[cmd])(cmd, args);
      else
        sout << "Unknown command " << cmd << endl;
    }
    catch(trap_t& t) {}
#ifdef HAVE_BOOST_ASIO
    wout(&bout); // socket output, if required
#endif
  }
  ctrlc_pressed = false;
}

void sim_t::interactive_help(const std::string& cmd, const std::vector<std::string>& args)
{
  sout <<
    "Interactive commands:\n"
    "reg <core> [reg]                # Display [reg] (all if omitted) in <core>\n"
    "fregh <core> <reg>              # Display half precision <reg> in <core>\n"
    "fregs <core> <reg>              # Display single precision <reg> in <core>\n"
    "fregd <core> <reg>              # Display double precision <reg> in <core>\n"
    "vreg <core> [reg]               # Display vector [reg] (all if omitted) in <core>\n"
    "pc <core>                       # Show current PC in <core>\n"
    "mem <hex addr>                  # Show contents of physical memory\n"
    "str <core> <hex addr>           # Show NUL-terminated C string at <hex addr> in core <core>\n"
    "until reg <core> <reg> <val>    # Stop when <reg> in <core> hits <val>\n"
    "until pc <core> <val>           # Stop when PC in <core> hits <val>\n"
    "untiln pc <core> <val>          # Run noisy and stop when PC in <core> hits <val>\n"
    "until mem <addr> <val>          # Stop when memory <addr> becomes <val>\n"
    "while reg <core> <reg> <val>    # Run while <reg> in <core> is <val>\n"
    "while pc <core> <val>           # Run while PC in <core> is <val>\n"
    "while mem <addr> <val>          # Run while memory <addr> is <val>\n"
    "run [count]                     # Resume noisy execution (until CTRL+C, or [count] insns)\n"
    "r [count]                         Alias for run\n"
    "rs [count]                      # Resume silent execution (until CTRL+C, or [count] insns)\n"
    "quit                            # End the simulation\n"
    "q                                 Alias for quit\n"
    "help                            # This screen!\n"
    "h                                 Alias for help\n"
    "Note: Hitting enter is the same as: run 1"
    << endl;
}

void sim_t::interactive_run_noisy(const std::string& cmd, const std::vector<std::string>& args)
{
  interactive_run(cmd,args,true);
}

void sim_t::interactive_run_silent(const std::string& cmd, const std::vector<std::string>& args)
{
  interactive_run(cmd,args,false);
}

void sim_t::interactive_run(const std::string& cmd, const std::vector<std::string>& args, bool noisy)
{
  size_t steps = args.size() ? atoll(args[0].c_str()) : -1;
  ctrlc_pressed = false;
  set_procs_debug(noisy);
  for (size_t i = 0; i < steps && !ctrlc_pressed && !done(); i++)
    step(1);
  if (!noisy) sout << ":" << endl;
}

void sim_t::interactive_quit(const std::string& cmd, const std::vector<std::string>& args)
{
  exit(0);
}

reg_t sim_t::get_pc(const std::vector<std::string>& args)
{
  if (args.size() != 1)
    throw trap_interactive();

  processor_t *p = get_core(args[0]);
  return p->get_state()->pc;
}

void sim_t::interactive_pc(const std::string& cmd, const std::vector<std::string>& args)
{
  if(args.size() != 1)
    throw trap_interactive();

  processor_t *p = get_core(args[0]);
  int max_xlen = p->get_max_xlen();
  sout << hex << setfill('0') << "0x" << setw(max_xlen/4)
       << zext(get_pc(args), max_xlen) << endl;
}

reg_t sim_t::get_reg(const std::vector<std::string>& args)
{
  if (args.size() != 2)
    throw trap_interactive();

  processor_t *p = get_core(args[0]);

  unsigned long r = std::find(xpr_name, xpr_name + NXPR, args[1]) - xpr_name;
  if (r == NXPR) {
    char *ptr;
    r = strtoul(args[1].c_str(), &ptr, 10);
    if (*ptr) {
      #define DECLARE_CSR(name, number) if (args[1] == #name) return p->get_csr(number);
      #include "encoding.h"              // generates if's for all csrs
      r = NXPR;                          // else case (csr name not found)
      #undef DECLARE_CSR
    }
  }

  if (r >= NXPR)
    throw trap_interactive();

  return p->get_state()->XPR[r];
}

freg_t sim_t::get_freg(const std::vector<std::string>& args)
{
  if(args.size() != 2)
    throw trap_interactive();

  processor_t *p = get_core(args[0]);
  int r = std::find(fpr_name, fpr_name + NFPR, args[1]) - fpr_name;
  if (r == NFPR)
    r = atoi(args[1].c_str());
  if (r >= NFPR)
    throw trap_interactive();

  return p->get_state()->FPR[r];
}

void sim_t::interactive_vreg(const std::string& cmd, const std::vector<std::string>& args)
{
  int rstart = 0;
  int rend = NVPR;
  if (args.size() >= 2) {
    rstart = strtol(args[1].c_str(), NULL, 0);
    if (!(rstart >= 0 && rstart < NVPR)) {
      rstart = 0;
    } else {
      rend = rstart + 1;
    }
  }

  // Show all the regs!
  processor_t *p = get_core(args[0]);
  const int vlen = (int)(p->VU.get_vlen()) >> 3;
  const int elen = (int)(p->VU.get_elen()) >> 3;
  const int num_elem = vlen/elen;
  sout << dec << "VLEN=" << (vlen << 3) << " bits; ELEN=" << (elen << 3) << " bits" << endl;

  for (int r = rstart; r < rend; ++r) {
    sout << setfill (' ') << left << setw(4) << vr_name[r] << right << ": ";
    for (int e = num_elem-1; e >= 0; --e){
      uint64_t val;
      switch(elen){
        case 8:
          val = P.VU.elt<uint64_t>(r, e);
          sout << dec << "[" << e << "]: 0x" << hex << setfill ('0') << setw(16) << val << "  ";
          break;
        case 4:
          val = P.VU.elt<uint32_t>(r, e);
          sout << dec << "[" << e << "]: 0x" << hex << setfill ('0') << setw(8) << (uint32_t)val << "  ";
          break;
        case 2:
          val = P.VU.elt<uint16_t>(r, e);
          sout << dec << "[" << e << "]: 0x" << hex << setfill ('0') << setw(8) << (uint16_t)val << "  ";
          break;
        case 1:
          val = P.VU.elt<uint8_t>(r, e);
          sout << dec << "[" << e << "]: 0x" << hex << setfill ('0') << setw(8) << (int)(uint8_t)val << "  ";
          break;
      }
    }
    sout << endl;
  }
}


void sim_t::interactive_reg(const std::string& cmd, const std::vector<std::string>& args)
{
  if (args.size() < 1)
     throw trap_interactive();

  processor_t *p = get_core(args[0]);
  int max_xlen = p->get_max_xlen();

  if (args.size() == 1) {
    // Show all the regs!

    sout << hex;
    for (int r = 0; r < NXPR; ++r) {
      sout << setfill(' ') << setw(4) << xpr_name[r]
           << ": 0x" << setfill('0') << setw(max_xlen/4)
           << zext(p->get_state()->XPR[r], max_xlen);
      if ((r + 1) % 4 == 0)
        sout << endl;
    }
  } else {
      sout << "0x" << setfill('0') << setw(max_xlen/4)
           << zext(get_reg(args), max_xlen) << endl;
  }
}

union fpr
{
  freg_t r;
  float s;
  double d;
};

void sim_t::interactive_freg(const std::string& cmd, const std::vector<std::string>& args)
{
  freg_t r = get_freg(args);
  sout << hex << "0x" << setfill ('0') << setw(16) << r.v[1] << setw(16) << r.v[0] << endl;
}

void sim_t::interactive_fregh(const std::string& cmd, const std::vector<std::string>& args)
{
  fpr f;
  f.r = freg(f16_to_f32(f16(get_freg(args))));
  sout << dec << (isBoxedF32(f.r) ? (double)f.s : NAN) << endl;
}

void sim_t::interactive_fregs(const std::string& cmd, const std::vector<std::string>& args)
{
  fpr f;
  f.r = get_freg(args);
  sout << dec << (isBoxedF32(f.r) ? (double)f.s : NAN) << endl;
}

void sim_t::interactive_fregd(const std::string& cmd, const std::vector<std::string>& args)
{
  fpr f;
  f.r = get_freg(args);
  sout << dec << (isBoxedF64(f.r) ? f.d : NAN) << endl;
}

reg_t sim_t::get_mem(const std::vector<std::string>& args)
{
  if (args.size() != 1 && args.size() != 2)
    throw trap_interactive();

  std::string addr_str = args[0];
  mmu_t* mmu = debug_mmu;
  if (args.size() == 2)
  {
    processor_t *p = get_core(args[0]);
    mmu = p->get_mmu();
    addr_str = args[1];
  }

  reg_t addr = strtol(addr_str.c_str(),NULL,16), val;
  if (addr == LONG_MAX)
    addr = strtoul(addr_str.c_str(),NULL,16);

  switch(addr % 8)
  {
    case 0:
      val = mmu->load_uint64(addr);
      break;
    case 4:
      val = mmu->load_uint32(addr);
      break;
    case 2:
    case 6:
      val = mmu->load_uint16(addr);
      break;
    default:
      val = mmu->load_uint8(addr);
      break;
  }
  return val;
}

void sim_t::interactive_mem(const std::string& cmd, const std::vector<std::string>& args)
{
  int max_xlen = procs[0]->get_max_xlen();

  sout << hex << "0x" << setfill('0') << setw(max_xlen/4)
       << zext(get_mem(args), max_xlen) << endl;
}

void sim_t::interactive_str(const std::string& cmd, const std::vector<std::string>& args)
{
  if (args.size() != 1 && args.size() != 2)
    throw trap_interactive();

  std::string addr_str = args[0];
  mmu_t* mmu = debug_mmu;
  if (args.size() == 2)
  {
    processor_t *p = get_core(args[0]);
    mmu = p->get_mmu();
    addr_str = args[1];
  }

  reg_t addr = strtol(addr_str.c_str(),NULL,16);

  char ch;
  while((ch = mmu->load_uint8(addr++)))
    sout << ch;

  sout << endl;
}

void sim_t::interactive_until_silent(const std::string& cmd, const std::vector<std::string>& args)
{
  interactive_until(cmd, args, false);
}

void sim_t::interactive_until_noisy(const std::string& cmd, const std::vector<std::string>& args)
{
  interactive_until(cmd, args, true);
}

void sim_t::interactive_until(const std::string& cmd, const std::vector<std::string>& args, bool noisy)
{
  bool cmd_until = cmd == "until" || cmd == "untiln";

  if (args.size() < 3)
    return;

  reg_t val = strtol(args[args.size()-1].c_str(),NULL,16);
  if (val == LONG_MAX)
    val = strtoul(args[args.size()-1].c_str(),NULL,16);

  // mask bits above max_xlen
  int max_xlen = procs[strtol(args[1].c_str(),NULL,10)]->get_max_xlen();
  if (max_xlen == 32) val &= 0xFFFFFFFF;

  std::vector<std::string> args2;
  args2 = std::vector<std::string>(args.begin()+1,args.end()-1);

  auto func = args[0] == "reg" ? &sim_t::get_reg :
              args[0] == "pc"  ? &sim_t::get_pc :
              args[0] == "mem" ? &sim_t::get_mem :
              NULL;

  if (func == NULL)
    return;

  ctrlc_pressed = false;

  while (1)
  {
    try
    {
      reg_t current = (this->*func)(args2);

      // mask bits above max_xlen
      if (max_xlen == 32) current &= 0xFFFFFFFF;

      if (cmd_until == (current == val))
        break;
      if (ctrlc_pressed)
        break;
    }
    catch (trap_t& t) {}

    set_procs_debug(noisy);
    step(1);
  }
}
