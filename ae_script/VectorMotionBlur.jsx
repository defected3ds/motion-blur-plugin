// Vector Motion Blur - After Effects script (runs the mbvideo tool on the selected footage layer).
// Use: File > Scripts > Run Script File...  (needs "Allow Scripts to Write Files and Access Network"
// in Preferences > Scripting & Expressions, and the mbvideo tool built from this project).
(function () {
    var SECTION = "PipelifeVectorMotionBlur";

    function getSetting(k, d) {
        return app.settings.haveSetting(SECTION, k) ? app.settings.getSetting(SECTION, k) : d;
    }
    function setSetting(k, v) { app.settings.saveSetting(SECTION, k, String(v)); }
    function shq(s) { return "'" + String(s).replace(/'/g, "'\\''") + "'"; }

    var comp = app.project.activeItem;
    if (!(comp instanceof CompItem)) { alert("Open a composition and select a footage layer first."); return; }
    if (comp.selectedLayers.length !== 1) { alert("Select exactly one footage layer."); return; }
    var layer = comp.selectedLayers[0];
    if (!(layer.source instanceof FootageItem) || !layer.source.file) {
        alert("The selected layer must be a video file (footage), not a comp, solid or image.\n" +
              "Tip: for pre-comps, render them to a movie first and import that.");
        return;
    }
    var srcFile = layer.source.file;

    // Where is mbvideo?
    var tool = getSetting("toolPath", "");
    if (!tool || !File(tool).exists) {
        var f = File.openDialog("Locate the 'mbvideo' tool (built with build_mac.command)");
        if (!f) return;
        tool = f.fsName;
        setSetting("toolPath", tool);
    }

    // Settings dialog
    var w = new Window("dialog", "Vector Motion Blur");
    w.orientation = "column"; w.alignChildren = "fill";
    function row(label, def) {
        var g = w.add("group");
        g.add("statictext", undefined, label).preferredSize.width = 130;
        var e = g.add("edittext", undefined, String(def)); e.characters = 8;
        return e;
    }
    var eShutter = row("Shutter angle", getSetting("shutter", 180));
    var ePhase = row("Shutter phase", getSetting("phase", -90));
    var eAmount = row("Blur amount %", getSetting("amount", 100));
    var cEdge = w.add("checkbox", undefined, "Edge-aware blur (experimental)");
    cEdge.value = getSetting("edge", "false") === "true";
    var bg = w.add("group"); bg.alignment = "right";
    bg.add("button", undefined, "Cancel", { name: "cancel" });
    bg.add("button", undefined, "Apply", { name: "ok" });
    if (w.show() !== 1) return;

    setSetting("shutter", eShutter.text); setSetting("phase", ePhase.text);
    setSetting("amount", eAmount.text); setSetting("edge", cEdge.value);

    var outPath = srcFile.fsName.replace(/\.[^.\/\\]+$/, "") + "_mb.mp4";
    var args = " --shutter " + parseFloat(eShutter.text) + " --phase " + parseFloat(ePhase.text) +
               " --amount " + parseFloat(eAmount.text) + (cEdge.value ? " --edge-aware" : "");
    // AE's shell has a minimal PATH; add the usual Homebrew locations so ffmpeg is found.
    var cmd = "/bin/bash -c " + shq("export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH; " +
              shq(tool) + " " + shq(srcFile.fsName) + " " + shq(outPath) + args + " 2>&1");

    // callSystem blocks until the tool is finished; AE looks frozen meanwhile.
    var result = system.callSystem(cmd);
    var outFile = new File(outPath);
    if (!outFile.exists || outFile.length === 0) {
        alert("mbvideo did not produce a file.\n\nOutput:\n" + result);
        return;
    }

    app.beginUndoGroup("Vector Motion Blur");
    var item = app.project.importFile(new ImportOptions(outFile));
    var nl = comp.layers.add(item);
    nl.startTime = layer.startTime;
    nl.inPoint = layer.inPoint; nl.outPoint = layer.outPoint;
    nl.moveAfter(layer);
    nl.name = layer.name + " (motion blur)";
    layer.enabled = false;  // original kept, just hidden
    app.endUndoGroup();
})();
