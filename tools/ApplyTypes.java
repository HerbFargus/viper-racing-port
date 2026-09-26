// Build the recovered class layouts (out/types.tsv, from tools/recover_types.py) into the Ghidra program:
// one structure per class, with its base class embedded first as super_<Base>, a typed vtable pointer,
// and its fields; one <Class>_vtbl structure of function pointers named after the slots' methods. The
// structures are the ones Ghidra uses for `this` in the class's __thiscall methods, and every empty
// placeholder the demangler made under the same name (for parameters such as `class PhobRoot *`) is
// replaced by the real one, so the decompiler shows field names everywhere.
//   analyzeHeadless <proj> ViperRacing -process race_v10.exe -noanalysis -scriptPath tools
//       -postScript ApplyTypes.java out\types.tsv
//@category ViperPort
import ghidra.app.script.GhidraScript;
import ghidra.app.util.NamespaceUtils;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.GhidraClass;
import ghidra.program.model.listing.VariableUtilities;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.*;

public class ApplyTypes extends GhidraScript {
    private DataTypeManager dtm;
    private final CategoryPath CAT = new CategoryPath("/ViperPort");
    private final Map<String, Structure> made = new HashMap<>();
    private int placed, skipped, replaced;

    /** The class namespace for "A::B", created or converted to a class as needed. */
    private GhidraClass classNamespace(String name) throws Exception {
        Namespace ns = currentProgram.getGlobalNamespace();
        for (String part : name.split("::")) {
            Namespace next = currentProgram.getSymbolTable().getNamespace(part, ns);
            if (next == null) next = currentProgram.getSymbolTable().getNamespace(part.replace(' ', '_'), ns);  // templates
            if (next == null) next = currentProgram.getSymbolTable().createClass(ns, part.replace(' ', '_'), SourceType.IMPORTED);
            ns = next;
        }
        if (!(ns instanceof GhidraClass)) ns = NamespaceUtils.convertNamespaceToClass(ns);
        return (GhidraClass) ns;
    }

    private DataType typeOf(String t) {
        switch (t) {
            case "float": return FloatDataType.dataType;
            case "double": return DoubleDataType.dataType;
            case "u1": return Undefined1DataType.dataType;
            case "u2": return Undefined2DataType.dataType;
            case "u4": return Undefined4DataType.dataType;
            case "u8": return Undefined8DataType.dataType;
            case "u10": return new ArrayDataType(Undefined1DataType.dataType, 10, 1);
            case "int": return IntegerDataType.dataType;
            case "uint": return UnsignedIntegerDataType.dataType;
            case "byte": return ByteDataType.dataType;
            case "char": return CharDataType.dataType;
            case "bool": return BooleanDataType.dataType;
            case "ptr": return PointerDataType.dataType;
        }
        if (t.startsWith("ptr:")) {                              // ptr:<type>, or ptr:<Class> (A::B allowed)
            String r = t.substring(4);
            boolean typed = r.startsWith("ptr:") || r.startsWith("struct:") || r.startsWith("arr:") || typeOf(r) != null;
            DataType to = typed ? typeOf(r) : typeOf("struct:" + r);
            return to == null ? PointerDataType.dataType : new PointerDataType(to);
        }
        if (t.startsWith("struct:")) return made.get(t.substring(7));
        if (t.startsWith("arr:")) {                              // arr:<element type>:<count>
            int c = t.lastIndexOf(':');
            DataType e = typeOf(t.substring(4, c));
            return e == null ? null : new ArrayDataType(e, Integer.parseInt(t.substring(c + 1)), e.getLength());
        }
        return null;
    }

    private final List<String> why = new ArrayList<>();

    private void skip(Structure s, int off, String name, String reason) {
        skipped++;
        if (why.size() < 400) why.add(s.getName() + " +0x" + Integer.toHexString(off) + " " + name + ": " + reason);
    }

    private void put(Structure s, int off, DataType dt, String name, String comment) {
        if (dt == null) { skip(s, off, name, "unknown type"); return; }
        if (dt.getLength() <= 0 || off < 0 || off + dt.getLength() > s.getLength()) {
            skip(s, off, name, "doesn't fit (" + dt.getName() + ", " + dt.getLength() + " bytes, struct " + s.getLength() + ")");
            return;
        }
        for (int i = off; i < off + dt.getLength(); i++) {
            DataTypeComponent c = s.getComponentContaining(i);
            if (c != null && c.getDataType() != DataType.DEFAULT && !(c.getDataType() instanceof Undefined)) {
                skip(s, off, name, "overlaps " + c.getFieldName() + " at +0x" + Integer.toHexString(c.getOffset()));
                return;
            }
        }
        try {
            s.replaceAtOffset(off, dt, dt.getLength(), name, comment);
            placed++;
        } catch (Exception ex) {
            skip(s, off, name, ex.toString());
        }
    }

    /** Swap every other empty structure/union called `name` (demangler placeholders) for ours. */
    private void replacePlaceholders(String name, DataType ours) {
        List<DataType> all = new ArrayList<>();
        dtm.findDataTypes(name, all);
        for (DataType d : all) {
            if (d == ours || d.getCategoryPath().equals(ours.getCategoryPath())) continue;
            boolean empty = (d instanceof Composite) && (((Composite) d).isNotYetDefined() || ((Composite) d).getNumDefinedComponents() == 0);
            if (!empty) continue;
            try {
                dtm.replaceDataType(d, ours, true);
                replaced++;
            } catch (Exception ex) {
                println("could not replace " + d.getPathName() + ": " + ex.getMessage());
            }
        }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File tsv = args.length > 0 ? new File(args[0]) : askFile("types.tsv from tools/recover_types.py", "Apply");
        dtm = currentProgram.getDataTypeManager();
        List<String[]> rows = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(tsv))) {
            String line;
            while ((line = r.readLine()) != null) if (!line.isEmpty()) rows.add(line.split("\t", -1));
        }
        Map<String, String> vtableOf = new HashMap<>();
        Map<String, List<String[]>> slots = new LinkedHashMap<>();
        for (String[] f : rows) {
            if (f[0].equals("class") && !f[4].equals("-")) vtableOf.put(f[1], f[4]);
            if (f[0].equals("vslot")) slots.computeIfAbsent(f[1], k -> new ArrayList<>()).add(f);
        }

        // vtable structures first: pointers to each slot's function signature
        Map<String, Structure> vtbl = new HashMap<>();
        for (Map.Entry<String, List<String[]>> e : slots.entrySet()) {
            List<String[]> ss = e.getValue();
            int len = Integer.parseInt(ss.get(ss.size() - 1)[2]) + 4;
            Structure v = new StructureDataType(CAT, e.getKey().replace("::", "_") + "_vtbl", len, dtm);
            for (String[] sl : ss) {
                Function fn = sl[3].equals("-") ? null : getFunctionAt(toAddr(Long.parseLong(sl[3], 16)));
                DataType ft = fn != null ? new PointerDataType(new FunctionDefinitionDataType(fn, false)) : PointerDataType.dataType;
                String nm = sl[4].replaceAll("[^A-Za-z0-9_]", "_").replaceAll("_+", "_").replaceAll("^_|_$", "");
                put(v, Integer.parseInt(sl[2]), ft, nm, sl[4]);
            }
            vtbl.put(e.getKey(), (Structure) dtm.addDataType(v, DataTypeConflictHandler.REPLACE_HANDLER));
        }

        // pass 1: every class and plain struct, at its size, with its base (or vtable pointer) at +0;
        // bases come first in the file, so each base is complete before it's embedded
        int classes = 0, globals = 0;
        List<Runnable> finish = new ArrayList<>();
        for (String[] f : rows) {
            if (!f[0].equals("plain") && !f[0].equals("class")) continue;
            String owner = f[1];
            int size = Integer.parseInt(f[2]);
            CategoryPath cat = CAT;
            String name = owner;
            if (f[0].equals("class")) {
                try {
                    Structure ph = VariableUtilities.findOrCreateClassStruct(classNamespace(owner), dtm);
                    cat = ph.getCategoryPath();
                    name = ph.getName();
                } catch (Exception ex) {
                    println("no class namespace for " + owner + " (" + ex.getMessage() + "): a plain structure instead");
                    name = owner.replaceAll("[^A-Za-z0-9_:]", "_");
                }
            }
            Structure cur = new StructureDataType(cat, name, Math.max(size, 1), dtm);
            if (f[0].equals("class")) {
                String base = f[3];
                if (!base.equals("-") && made.containsKey(base)) {
                    put(cur, 0, made.get(base), "super_" + base.replace("::", "_"), "base class");
                } else if (!f[4].equals("-")) {
                    DataType vt = vtbl.get(owner);
                    put(cur, 0, vt != null ? new PointerDataType(vt) : PointerDataType.dataType, "vtable", null);
                }
                classes++;
            }
            cur = (Structure) dtm.addDataType(cur, DataTypeConflictHandler.REPLACE_HANDLER);
            made.put(owner, cur);
            final Structure s = cur;
            finish.add(() -> replacePlaceholders(owner.contains("::") ? owner.substring(owner.lastIndexOf(':') + 1) : owner, s));
        }

        // pass 2: fields (any struct may now be embedded in any other), then typed globals
        for (String[] f : rows) {
            if (f[0].equals("field") && made.containsKey(f[1])) {
                DataType dt = typeOf(f[3]);
                String name = f[4].equals("-") ? null : f[4];
                put(made.get(f[1]), Integer.parseInt(f[2]), dt, name, f.length > 5 && !f[5].isEmpty() ? f[5] : null);
            } else if (f[0].equals("global")) {
                Address a = toAddr(Long.parseLong(f[1], 16));
                DataType dt = typeOf(f[2]);
                if (dt == null) continue;
                try {
                    clearListing(a, a.add(dt.getLength() - 1));
                    createData(a, dt);
                    Symbol sym = getSymbolAt(a);
                    if (sym == null || sym.getSource() == SourceType.DEFAULT)
                        createLabel(a, f[3], true);
                    if (f.length > 4 && !f[4].isEmpty()) setEOLComment(a, f[4]);
                    globals++;
                } catch (Exception ex) {
                    println("could not type global " + f[3] + ": " + ex.getMessage());
                }
            }
        }
        for (Runnable r : finish) r.run();
        if (getScriptArgs().length > 1)
            try (java.io.PrintWriter w = new java.io.PrintWriter(getScriptArgs()[1], "UTF-8")) { for (String x : why) w.println(x); }
        println("ApplyTypes: " + classes + " classes, " + vtbl.size() + " vtables; " + placed + " fields placed, "
                + skipped + " skipped (overlapping or out of range), " + replaced + " placeholders replaced, "
                + globals + " globals typed");
    }
}
