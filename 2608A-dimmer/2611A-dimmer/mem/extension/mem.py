
#------------------------------------------------------------------------
# python3 -m mem mem.py
#------------------------------------------------------------------------

import gdb
import subprocess
import os
import sys
import numpy as np
import inspect

np.set_printoptions(linewidth=256)
np.set_printoptions(suppress=True)

Path = 'mem/data'
os.makedirs(Path, exist_ok=True) 

LOG = 0

def PRINT(*args, **kwargs):
    print(inspect.currentframe().f_back.f_lineno, *args, **kwargs)

# Value  Constant                     Meaning
# -1     TYPE_CODE_BITSTRING          Bit string (deprecated, kept for compatibility)
# 1      TYPE_CODE_PTR                Pointer, e.g. T*
# 2      TYPE_CODE_ARRAY              Array, e.g. T[N]
# 3      TYPE_CODE_STRUCT             Struct or C++ class
# 4      TYPE_CODE_UNION              Union
# 5      TYPE_CODE_ENUM               Enumeration
# 6      TYPE_CODE_FLAGS              Bit-flags register type (e.g. eflags)
# 7      TYPE_CODE_FUNC               Function type
# 8      TYPE_CODE_INT                Integer (int, long, size_t, ...)
# 9      TYPE_CODE_FLT                Floating point (float, double)
# 10     TYPE_CODE_VOID               void
# 11     TYPE_CODE_SET                Set type (Pascal / Modula-2)
# 12     TYPE_CODE_RANGE              Range type (array index bounds, etc.)
# 13     TYPE_CODE_STRING             String type (Fortran-style, NOT char*)
# 14     TYPE_CODE_ERROR              Unrecognized / corrupt debug info
# 15     TYPE_CODE_METHOD             C++ member function type
# 16     TYPE_CODE_METHODPTR          Pointer to member function, void (T::*)()
# 17     TYPE_CODE_MEMBERPTR          Pointer to data member, int T::*
# 18     TYPE_CODE_REF                C++ lvalue reference, T&
# 19     TYPE_CODE_RVALUE_REF         C++ rvalue reference, T&&
# 20     TYPE_CODE_CHAR               Character type, char
# 21     TYPE_CODE_BOOL               Boolean, bool
# 22     TYPE_CODE_COMPLEX            Complex number, _Complex
# 23     TYPE_CODE_TYPEDEF            typedef / using alias (not yet stripped)
# 24     TYPE_CODE_NAMESPACE          C++ namespace
# 25     TYPE_CODE_DECFLOAT           Decimal float (_Decimal64, etc.)
# 26     TYPE_CODE_MODULE             Module (Fortran)
# 27     TYPE_CODE_INTERNAL_FUNCTION  GDB internal convenience function ($_strlen, ...)
# 28     TYPE_CODE_XMETHOD            Python-defined extension method (xmethod)
# 29     TYPE_CODE_FIXED_POINT        Fixed-point number type
# 30     TYPE_CODE_NAMELIST           Fortran namelist

_CPU_MAP = {
    'unsigned char':            np.uint8,
    'char':                     np.int8,
    'unsigned short':           np.uint16,      
    'short':                    np.int16,
    'unsigned int':             np.uint32,        
    'int':                      np.int32,
    'unsigned long long':       np.uint64,
    'unsigned long':            np.dtype('L'),
    'float':                    np.float32,
    'double':                   np.float64,       
}

def get_pids(name):
    pids = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                parts = f.read().split(b"\x00")
            exe = os.path.basename(parts[0].decode(errors="ignore"))
            if exe == name:
                pids.append(int(pid))
        except (FileNotFoundError, PermissionError):
            continue

    if not pids:
        print(f"get_pids({name}) fail")
        sys.exit()
    return pids

def read_memory(pid, addr, size):
    with open(f"/proc/{pid}/mem", "rb", 0) as f:
        f.seek(addr)    
        return f.read(size)

def mon(app, addr, type, size):
    r = subprocess.run(
        # sudo sysctl kernel.yama.ptrace_scope=0
        ["sudo", "sysctl", "kernel.yama.ptrace_scope=0"]#, capture_output=True, text=True
    )
    if r.returncode != 0:
        print("sudo sysctl kernel.yama.ptrace_scope=0 fail")  
        sys.exit()

    pid = get_pids(app)[0]
    addr_t = int(addr, 0) if isinstance(addr, str) else int(addr)

    shape = size if isinstance(size, (list, tuple)) else [size]
    data_size = 1
    for s in shape:
        data_size *= s
    data_size *= np.dtype(_CPU_MAP[type]).itemsize

    raw = read_memory(pid, addr_t, data_size)
    data = np.frombuffer(raw, dtype=_CPU_MAP[type])

    if len(shape) > 1:
        data = data.reshape(shape)
    return data

#------------------------------------------------------------------------
# cpu
#------------------------------------------------------------------------

def read(Addr, Type, Size):
    shape = Size if isinstance(Size, (list, tuple)) else [Size]
    data_size = 1
    for s in shape:
        data_size *= int(s)
    data_size *= np.dtype(_CPU_MAP[Type]).itemsize

    inferior = gdb.selected_inferior()
    raw = inferior.read_memory(Addr, data_size)
    data = np.frombuffer(raw, dtype=_CPU_MAP[Type])

    if len(shape) > 1:
        data = data.reshape(shape)
    return data

def cpu(Name, Type=None, Size=None):   
#    r = subprocess.run(
#        # sudo sysctl kernel.yama.ptrace_scope=0
#        ["sudo", "sysctl", "kernel.yama.ptrace_scope=0"]#, capture_output=True, text=True
#    )
#    if r.returncode != 0:
#        print("sudo sysctl kernel.yama.ptrace_scope=0 fail")  
#        sys.exit()

    para = gdb.parse_and_eval(Name)
    t = para.type.strip_typedefs()
 
    while t.code in (gdb.TYPE_CODE_REF, gdb.TYPE_CODE_RVALUE_REF):
        para = para.referenced_value()
        t = para.type.strip_typedefs()

    if t.code == gdb.TYPE_CODE_PTR: # 1 
        if str(t).startswith("std::vector") or str(t).startswith("const std::vector"):
            para = para.dereference()
            data_addr = int(para['_M_impl']['_M_start'])

            start = para['_M_impl']['_M_start']
            finish = para['_M_impl']['_M_finish']
            Size_t = int(finish - start)
            Size = Size_t if Size is None else min(Size, Size_t)

            return read(data_addr, Type, Size)   

        elif str(t).startswith("std::array") or str(t).startswith("const std::array"):
            para = para.dereference()
            data_addr = int(para['_M_elems'].address)
            return read(data_addr, Type, Size)

        elif str(t).startswith("std::pair") or str(t).startswith("const std::pair"):
            para = para.dereference()
            data_addr = int(para.address)
            return read(data_addr, Type, Size)

        elif 'basic_string' in str(t):
            n = int(para['_M_string_length'])
            s = para['_M_dataplus']['_M_p'].string(length=n)
            return s

        else:
            data_addr = int(para)
            return read(data_addr, Type, Size)
        
    elif t.code == gdb.TYPE_CODE_ARRAY: # 2    
        if Type not in _CPU_MAP:
            if len(Size) == 1:   
                results = [None] * Size  
                for i in range(Size[0]):
                    results[i] = cpu(f'{Name}[{i}]')
                return results;

            elif len(Size) == 2:   
                results = [None] * (Size[0], Size[1])
                for i in range(Size[0]):
                    for j in range(Size[1]):
                        results[i][j] = cpu(f'{Name}[{i}][{j}]')
                return results;
        else:
            data_addr = int(para.address)
            return read(data_addr, Type, Size)      

    elif t.code == gdb.TYPE_CODE_STRUCT: # 3
        if str(t).startswith("std::vector") or str(t).startswith("const std::vector"):
            Type = str(t.template_argument(0)) if Type is None else Type

            start = para['_M_impl']['_M_start']
            finish = para['_M_impl']['_M_finish']
            Size_t = int(finish - start)
            Size = Size_t if Size is None else min(Size, Size_t)

            if Type not in _CPU_MAP:
                elem = gdb.parse_and_eval(Name).type.strip_typedefs().template_argument(0).strip_typedefs()
                names = [f.name for f in elem.fields() if not f.is_base_class and f.bitpos is not None]
                results = {name: [None] * Size for name in names}

                if LOG: PRINT('names', names)

                for i in range(Size):
                    result = cpu(f'{Name}[{i}]')
                    for name in names:
                        results[name][i] = result[name]

                results = {name: np.array(vals) for name, vals in results.items()}
                return results
            else:     
                data_addr = int(para['_M_impl']['_M_start'])
                return read(data_addr, Type, Size)    

        elif str(t).startswith("std::array") or str(t).startswith("const std::array"):
            data_addr = int(para['_M_elems'].address)
            return read(data_addr, Type, Size)    

        elif str(t).startswith("std::pair") or str(t).startswith("const std::pair"):
            data_addr = int(para.address)
            return read(data_addr, Type, Size)    

        elif str(t).startswith("Eigen::Matrix") or str(t).startswith("const Eigen::Matrix"):
            n = int(t.template_argument(1))
            d = para['m_storage']['m_data']
            data_addr = int(d['array'][0].address)
            return read(data_addr, Type, Size) 

        elif '::basic_string' in str(t): # std::string
            n = int(para['_M_string_length'])
            s = para['_M_dataplus']['_M_p'].string(length=n)
            return np.str_(s)        
            
        else:
            names = [f.name for f in t.fields() if not f.is_base_class and f.bitpos is not None]
            results = {name: [] for name in names}

            PRINT('names', names)
            
            for name in names:
                results[name] = cpu(f'{Name}.{name}', None, None)

            return results

    elif t.code == gdb.TYPE_CODE_INT: # 8
        if LOG: PRINT('t.code', gdb.TYPE_CODE_INT)
        if t.sizeof == 1:
            return np.int8(para) if t.is_signed else np.uint8(para)
        elif t.sizeof == 2:
            return np.int16(para) if t.is_signed else np.uint16(para)
        elif t.sizeof == 4:
            return np.int32(para) if t.is_signed else np.uint32(para)
        else:
            return np.int64(para) if t.is_signed else np.uint64(para)

    elif t.code == gdb.TYPE_CODE_FLT: # 9
        if t.sizeof == 4:
            return np.float32(para)
        elif t.sizeof == 8:
            return np.float64(para)
        else:
            return np.longdouble(para)

    else:
        if LOG: PRINT(f"{name}({t.code}): fail")

    return None
    
#------------------------------------------------------------------------
# gpu
#------------------------------------------------------------------------

_GPU_MAP = {
    'unsigned char':            np.uint8,
    'char':                     np.int8,
    'unsigned short':           np.uint16,      
    'short':                    np.int16,
    'unsigned int':             np.uint32,        
    'int':                      np.int32,
    'unsigned long long':       np.uint64,
    'unsigned long':            np.dtype('L'),
    'float':                    np.float32,
    'double':                   np.float64,       
}

def gpu(Name, Type=None, Size=12, Step = 1):  
    para = gdb.parse_and_eval(Name)
    t = para.type.strip_typedefs()

    while t.code in (gdb.TYPE_CODE_REF, gdb.TYPE_CODE_RVALUE_REF):
        para = para.referenced_value()
        t = para.type.strip_typedefs()   

    if t.code == gdb.TYPE_CODE_PTR: # 1 
        Type = str(t.target()) if Type is None else Type
        
        type_t = None
        for name, dtype in _GPU_MAP.items():
            if name in Type:
                type_t = dtype
                break

        if type_t is None:
            PRINT(f'mem do not support {Type} fail')
            return None;
        
        size_t = Size if isinstance(Size, (list, tuple)) else [Size]

        data_size = int(np.prod(size_t, dtype=np.int64))
        data_size *= np.dtype(type_t).itemsize

        PRINT('Type', Type)
        
        if '@' not in str(t.target()):
            d_addr = int(para)

            expr = f"(unsigned char*)malloc({data_size})"
            if LOG: PRINT('expr', expr)

            h_addr = int(gdb.parse_and_eval(expr))

            if LOG: PRINT('h_addr', h_addr, 'd_addr', d_addr, 'data_size', data_size)

            expr = f"((int (*)(void*, const void*, size_t, int))cudaMemcpy)((void*){h_addr}, (void*){d_addr}, {data_size}, cudaMemcpyDeviceToHost)"
            ret = gdb.parse_and_eval(expr)

            inferior = gdb.selected_inferior()
            raw = inferior.read_memory(h_addr, data_size)
            data = np.frombuffer(raw, dtype=type_t).copy()

            expr = f"((void(*)(void*))free)((void*){h_addr})"
            gdb.parse_and_eval(expr)

        elif Step > 1 and len(shape) > 1:
            data = np.empty(shape, dtype=type_t)

            for i in range(0, shape[0], Step):
                for j in range(0, shape[1], Step):
                    data[i, j] = (para + i * shape[1] + j).dereference()

                    for di in range(Step): 
                        ii = i + di
                        if ii >= shape[0]:
                            continue

                        for dj in range(Step):
                            jj = j + dj
                            if jj >= shape[1]:
                                continue
                            
                            data[ii, jj] = data[i, j];

        else:
            total_size = data_size
            if data_size % 8 == 0:
                total_size = int(data_size / 8)
                total_data = np.empty(total_size, dtype=np.ulonglong)
                total_type = gdb.lookup_type("unsigned long long")
            elif data_size % 4 == 0:
                total_size = int(data_size / 4)
                total_data = np.empty(total_size, dtype=np.uint32)
                total_type = gdb.lookup_type("unsigned int")
            elif data_size % 2 == 0:
                total_size = int(data_size / 2)
                total_data = np.empty(total_size, dtype=np.uint16)
                total_type = gdb.lookup_type("unsigned short")    
            else:
                total_size = int(data_size)
                total_data = np.empty(total_size, dtype=np.uint8)
                total_type = gdb.lookup_type("unsigned char")  

            for i in range(total_size):
                total_offset = int(i * total_data.itemsize / para.type.target().sizeof)
                total_data[i] = (para + total_offset).cast(total_type.pointer()).dereference()
        
            data = total_data.view(type_t)


        if len(size_t) > 1:
            data = data.reshape(size_t)
        return data
  
    elif t.code == gdb.TYPE_CODE_INT: # 8
        if LOG: PRINT('t.code', gdb.TYPE_CODE_INT)
        if t.sizeof == 1:
            return np.int8(para) if t.is_signed else np.uint8(para)
        elif t.sizeof == 2:
            return np.int16(para) if t.is_signed else np.uint16(para)
        elif t.sizeof == 4:
            return np.int32(para) if t.is_signed else np.uint32(para)
        else:
            return np.int64(para) if t.is_signed else np.uint64(para)

    elif t.code == gdb.TYPE_CODE_FLT: # 9
        if t.sizeof == 4:
            return np.float32(para)
        elif t.sizeof == 8:
            return np.float64(para)
        else:
            return np.longdouble(para)

    else:
        if LOG: PRINT(f"{name}({t.code}): fail")

    return None
    

#------------------------------------------------------------------------
# mem
#------------------------------------------------------------------------

def mem(Name, Type=None, Size=None, Step=1):
    _FULL_TYPE = {
        'uc':                   'unsigned char',
        'c':                    'char',
        'us':                   'unsigned short',
        's':                    'short',
        'ui':                   'unsigned int',
        'i':                    'int',
        'ull':                  'unsigned long long',
        'ul':                   'unsigned long',
        'f':                    'f',
        'd':                    'double',
    }

    if Type is not None and Type in _FULL_TYPE:
        Type = _FULL_TYPE[Type]  

    if Type == None and Size == None:
        if LOG: PRINT('mem cpu')
        data = cpu(Name, Type, Size)
        if LOG: PRINT('type(data)', type(data))
        if LOG: PRINT('mem cpu save', f'{Path}/{Name}.npy')
        np.save(f'{Path}/{Name}.npy', data) 
        return data

    if LOG: PRINT('Name', Name)

    if LOG: PRINT('repr(Name)', repr(Name))

    para = gdb.parse_and_eval(Name)

    t = para.type.strip_typedefs()

    if LOG: PRINT('para.type.strip_typedefs()', str(t))

    if '@' not in str(t):
        buf = int(gdb.parse_and_eval("(void *) malloc(64)"))
        rc  = int(gdb.parse_and_eval(f"(int) cudaPointerGetAttributes((void *) {buf:#x}, {Name})"))
        t   = int(gdb.parse_and_eval(f"*(int *) {buf:#x}"))

        if LOG: PRINT("rc =", rc, " type =", t)
        if LOG: PRINT({0: "host-malloc", 1: "pinned", 2: "device", 3: "managed"}.get(t, "?"))

        gdb.parse_and_eval(f"(void) free((void *) {buf:#x})")

        if t in [2, 3]:
            if LOG: PRINT('mem gpu host')
            data = gpu(Name, Type, Size, Step)
            if LOG: PRINT('mem gpu host save', f'{Path}/{Name}.npy')
            np.save(f'{Path}/{Name}.npy', data) 
            return data
        else:
            if LOG: PRINT('mem cpu')
            data = cpu(Name, Type, Size)
            if LOG: PRINT('mem cpu save', f'{Path}/{Name}.npy')
            np.save(f'{Path}/{Name}.npy', data) 
            return data
    
    else:        
        if LOG: PRINT('mem gpu device')
        data = gpu(Name, Type, Size, Step)
        if LOG: PRINT('mem gpu device save', f'{Path}/{Name}.npy')
        np.save(f'{Path}/{Name}.npy', data) 
        return data

    return None

'''
#------------------------------------------------------------------------
Credit balance is too low
unset ANTHROPIC_API_KEY
echo "[$ANTHROPIC_API_KEY]"
/login "Claude account with subscription"
/status
/usage

#------------------------------------------------------------------------
Jul 15 2026:

-exec source /home/roots/mem/pp.py

import os
import sys
import importlib

mem_dir = os.path.abspath('/home/roots/mem')
if mem_dir not in sys.path: sys.path.insert(0, mem_dir)

import mem
importlib.reload(mem)

#from mem import cpu, gpu, imtool, plot, addr
from mem import*

addr('A')
img = cpu('inputImg', 'uc', [480, 640])
imtool(img)

#------------------------------------------------------------------------
Jul 16 2026:

1)
~/.vscode/extensions

extension.js
package.json

node --version
npm --version

sudo npm install -g @vscode/vsce

bash install.sh

vsce package --allow-missing-repository --skip-license
code --uninstall-extension local.f8comma
code --install-extension f8comma-0.0.1.vsix

Or:
Ctrl + Shit + P
Extensions: Install from VSIX...

2)
/home/roots/.config/Code/User/keybindings.json

    {
    	"key": "f7",
    	"command": "editor.debug.action.selectionToRepl",
    	"when": "editorHasSelection && inDebugMode"
    },
    {
        "key": "f8",
        "command": "f8run.comma",
        "when": "editorTextFocus && inDebugMode"
    }

3)
Ctrl + Shift + P
Reload Window

Or:
Ctrl + Shift + P
Extension Development Host

#------------------------------------------------------------------------
'''
