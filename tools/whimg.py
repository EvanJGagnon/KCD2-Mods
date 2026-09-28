# whimg.py -- load WHGame.dll as a mapped image for signature work.
import pefile, capstone, bisect, os
GAME = os.path.join(os.environ.get("KCD2_DIR", r"C:\Program Files (x86)\Steam\steamapps\common\KingdomComeDeliverance2"), "Bin", "Win64MasterMasterSteamPGO", "WHGame.dll")
class Img:
    def __init__(self, path=GAME):
        self.pe = pefile.PE(path, fast_load=True)
        self.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
        self.mem = self.pe.get_memory_mapped_image()
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        t = [s for s in self.pe.sections if s.Name.rstrip(b'\0') == b'.text'][0]
        self.text = (t.VirtualAddress, t.VirtualAddress + t.Misc_VirtualSize)
        self.funcs = sorted((e.struct.BeginAddress, e.struct.EndAddress) for e in self.pe.DIRECTORY_ENTRY_EXCEPTION)
        self.fstarts = [f[0] for f in self.funcs]
        self.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); self.cs.detail = True
    def func_of(self, rva):
        i = bisect.bisect_right(self.fstarts, rva) - 1
        return self.funcs[i] if i >= 0 and self.funcs[i][0] <= rva < self.funcs[i][1] else None
    def insns(self, rva, n=40):
        return list(self.cs.disasm(bytes(self.mem[rva:rva + 16 * n]), rva))[:n]
    def dis(self, rva, n=20, mark=None):
        for i in self.insns(rva, n):
            print(("=>" if i.address == mark else "  "), hex(i.address), i.bytes.hex().ljust(24), i.mnemonic, i.op_str)
