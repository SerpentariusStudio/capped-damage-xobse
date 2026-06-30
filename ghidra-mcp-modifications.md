# GhidraMCP Modifications — `read_memory` / `read_pointer`

Stock GhidraMCP (LaurieWired) exposes decompilation, disassembly, xrefs, strings, and
listing data — but **no raw memory read**. That blocked a core RE need: *dereferencing
pointers* (following a vtable slot `vtable_base + index*8`, or a struct's function-pointer
field) to the address it points at. The decompiler shows `(*(code **)(*actor + 0x5B8))(...)`
but not the concrete target function; resolving it requires reading the 8 bytes at that slot.

This document records the two endpoints we added and exactly how to rebuild/redeploy them.

---

## What was added

Two HTTP endpoints in the Ghidra-side Java plugin, plus matching MCP tools in the Python bridge:

| Tool | Args | Returns |
|---|---|---|
| `read_memory` | `address`, `length` (default 16, max 4096) | raw bytes as hex + an ASCII gutter |
| `read_pointer` | `address` | 8-byte little-endian value at `address`, **and resolves the target** to `-> function <name>` / `-> data <label>` / `-> inside function <name>` |

`read_pointer` is the important one: it's the "follow a pointer" primitive. To read vtable
slot `N`: `read_pointer(vtable_base + N*8)`.

---

## Architecture (why both layers changed)

```
Claude (MCP client)
   │  MCP (stdio)
   ▼
bridge_mcp_ghidra.py        ← thin forwarder; one @mcp.tool() per HTTP endpoint
   │  HTTP GET 127.0.0.1:8080
   ▼
GhidraMCP.jar (plugin inside Ghidra)   ← embedded HttpServer; actually touches Ghidra API
   │
   ▼
Ghidra Program / Memory
```

The capability has to live in the **Java plugin** (only it can call `program.getMemory()`).
The Python bridge just needs a wrapper so the tool is exposed over MCP. So both files change.

---

## Files

- **Modified plugin source** (cloned from upstream `main`):
  `E:\Programs\Ghidra\GhidraMCP-src\src\main\java\com\lauriewired\GhidraMCPPlugin.java`
  - Added two `server.createContext("/read_memory" | "/read_pointer", …)` handlers (next to `/strings`).
  - Added `private String readMemory(String,int)` and `private String readPointer(String)` (next to `getCurrentProgram()`), both using `program.getMemory().getBytes(addr, buf)`.
- **Live MCP bridge** Claude Code launches for the `ghidra` server:
  `E:\Programs\Ghidra\GhidraMCP-release-1-4\GhidraMCP-release-1-4\bridge_mcp_ghidra.py`
  - Added `@mcp.tool() read_memory(...)` and `read_pointer(...)` (safe_get to `/read_memory`, `/read_pointer`).
- **Installed plugin jar** (replaced in place):
  `C:\Users\Admin\AppData\Roaming\ghidra\ghidra_12.0.1_PUBLIC\Extensions\GhidraMCP\lib\GhidraMCP.jar`
  - Original backed up alongside as `GhidraMCP.jar.bak-<timestamp>`.

---

## The two added methods (Java)

```java
private String readMemory(String addressStr, int length) {
    Program program = getCurrentProgram();
    if (program == null) return "No program loaded";
    if (addressStr == null || addressStr.isEmpty()) return "Address is required";
    if (length <= 0 || length > 4096) length = 16;
    try {
        Address addr = program.getAddressFactory().getAddress(addressStr);
        byte[] buf = new byte[length];
        int n = program.getMemory().getBytes(addr, buf);
        StringBuilder hex = new StringBuilder();
        StringBuilder asc = new StringBuilder();
        for (int i = 0; i < n; i++) {
            hex.append(String.format("%02X ", buf[i] & 0xFF));
            char c = (char) (buf[i] & 0xFF);
            asc.append((c >= 32 && c < 127) ? c : '.');
        }
        return String.format("%s: %s| %s", addr, hex.toString(), asc.toString());
    } catch (Exception e) {
        return "Error reading memory: " + e.getMessage();
    }
}

private String readPointer(String addressStr) {
    Program program = getCurrentProgram();
    if (program == null) return "No program loaded";
    if (addressStr == null || addressStr.isEmpty()) return "Address is required";
    try {
        Address addr = program.getAddressFactory().getAddress(addressStr);
        byte[] buf = new byte[8];
        program.getMemory().getBytes(addr, buf);
        long value = 0;
        for (int i = 7; i >= 0; i--) value = (value << 8) | (buf[i] & 0xFFL);
        String hex = String.format("0x%x", value);
        String target = "";
        try {
            Address tgt = program.getAddressFactory().getAddress(Long.toHexString(value));
            Function f = program.getFunctionManager().getFunctionAt(tgt);
            if (f != null) {
                target = " -> function " + f.getName() + " @ " + tgt;
            } else {
                Data d = program.getListing().getDataAt(tgt);
                if (d != null) {
                    String label = d.getLabel() != null ? d.getLabel() : d.getPathName();
                    target = " -> data " + label + " = " + d.getDefaultValueRepresentation();
                } else {
                    Function cf = program.getFunctionManager().getFunctionContaining(tgt);
                    if (cf != null) target = " -> inside function " + cf.getName();
                }
            }
        } catch (Exception ignore) { }
        return String.format("%s: %s%s", addr, hex, target);
    } catch (Exception e) {
        return "Error reading pointer: " + e.getMessage();
    }
}
```

The bridge wrappers (Python):

```python
@mcp.tool()
def read_memory(address: str, length: int = 16) -> str:
    return "\n".join(safe_get("read_memory", {"address": address, "length": length}))

@mcp.tool()
def read_pointer(address: str) -> str:
    return "\n".join(safe_get("read_pointer", {"address": address}))
```

---

## Rebuild recipe (no Maven/Gradle needed)

Environment: JDK 21 at `E:\Programs\Java`; Ghidra root
`E:\Programs\Ghidra\ghidra_12.0.1_PUBLIC_20260114\ghidra_12.0.1_PUBLIC` (209 jars).

PowerShell:

```powershell
$ghidra = "E:\Programs\Ghidra\ghidra_12.0.1_PUBLIC_20260114\ghidra_12.0.1_PUBLIC"
# FORWARD slashes in the argfile — backslashes are eaten as escapes by javac @files
$jars = (Get-ChildItem -Path $ghidra -Recurse -Filter *.jar | ForEach-Object { $_.FullName -replace '\\','/' }) -join ';'
$src  = "E:/Programs/Ghidra/GhidraMCP-src/src/main/java/com/lauriewired/GhidraMCPPlugin.java"
$out  = "E:\Programs\Ghidra\GhidraMCP-src\build_out"
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force $out | Out-Null
$argfile = "E:\Programs\Ghidra\GhidraMCP-src\javac_args.txt"
Set-Content -Path $argfile -Value ("-cp `"$jars`"`n`"$src`"") -Encoding ASCII
& javac -d $out "@$argfile"          # exit 0 (only deprecation warnings)

# `javac` on PATH is the Oracle javapath shim (no jar.exe). Get the real jar.exe:
$jh = (& java -XshowSettings:properties 2>&1 | Select-String 'java.home').ToString().Split('=')[1].Trim()  # E:\Programs\Java
$jarexe = Join-Path $jh "bin\jar.exe"
$jar = "E:\Programs\Ghidra\GhidraMCP-src\GhidraMCP.jar"
& $jarexe --create --file $jar --manifest "E:\Programs\Ghidra\GhidraMCP-src\src\main\resources\META-INF\MANIFEST.MF" -C $out .

# Install over the live jar (original auto-backed-up the first time):
Copy-Item $jar "C:\Users\Admin\AppData\Roaming\ghidra\ghidra_12.0.1_PUBLIC\Extensions\GhidraMCP\lib\GhidraMCP.jar" -Force
```

---

## Activating changes

Any time the Java side changes:
1. **Restart Ghidra** (reopen project + program; analysis is saved). On startup the log should show
   `GhidraMCP HTTP server started on port 8080`.
2. **Reconnect the `ghidra` MCP server** in Claude Code (`/mcp` → reconnect, or restart the session)
   so the bridge re-registers and any new tools appear.

If only the Python bridge changed (e.g. added a wrapper for an endpoint that already exists),
only step 2 is needed.

---

## Gotchas hit during the build

- **javac @argfile + backslashes**: `E:\Programs\…` became `E:Programs…` (backslash = escape). Use forward slashes in the argfile.
- **`jar` not on PATH**: `javac`/`java` resolve to the Oracle `…\Common Files\Oracle\Java\javapath\` shim, which has no `jar.exe`. Use `jar.exe` from the real `java.home` (`E:\Programs\Java\bin`).
- **Ghidra root is nested**: `…ghidra_12.0.1_PUBLIC_20260114\ghidra_12.0.1_PUBLIC\…`.
- The rebuilt jar is smaller than the original (~22.5 KB vs ~27.9 KB) only because it omits Maven
  metadata under `META-INF/maven/` — irrelevant to Ghidra, which discovers the plugin via `@PluginInfo` scanning.
