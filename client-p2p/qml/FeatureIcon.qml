import QtQuick

// Original vector drawings: no icon font, network request or bitmap download.
Canvas {
    property string glyph: ""
    property color tint: "#1685ef"
    implicitWidth: 20
    implicitHeight: 20
    Accessible.name: glyph
    onGlyphChanged: requestPaint()
    onTintChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    function line(c, points, close) {
        c.beginPath(); c.moveTo(points[0][0], points[0][1]);
        for (let i = 1; i < points.length; ++i) c.lineTo(points[i][0], points[i][1]);
        if (close) c.closePath();
        c.stroke();
    }
    function circle(c, x, y, r) { c.beginPath(); c.arc(x, y, r, 0, Math.PI * 2); c.stroke(); }
    onPaint: {
        const c = getContext("2d");
        c.clearRect(0, 0, width, height); c.save(); c.scale(width / 24, height / 24);
        c.strokeStyle = tint; c.fillStyle = tint; c.lineWidth = 1.8; c.lineCap = "round"; c.lineJoin = "round";
        switch (glyph) {
        case "account": case "friend": case "avatar":
            circle(c, glyph === "friend" ? 9 : 12, 7, 3.5);
            c.beginPath(); c.moveTo(4, 21); c.bezierCurveTo(4, 12, 19, 12, 20, 21); c.stroke();
            if (glyph === "friend") { line(c, [[18,8],[18,14]]); line(c, [[15,11],[21,11]]); }
            break;
        case "group":
            circle(c, 9, 7, 3); circle(c, 18, 8, 2.5);
            c.beginPath(); c.moveTo(2,20); c.bezierCurveTo(2,12,16,12,16,20); c.stroke();
            c.beginPath(); c.moveTo(17,14); c.bezierCurveTo(21,14,22,17,22,20); c.stroke(); break;
        case "file":
            line(c, [[3,19],[3,5],[9,5],[12,8],[21,8],[21,19],[3,19]], true); break;
        case "emoji":
            circle(c,12,12,9); circle(c,9,9,.5); circle(c,15,9,.5);
            c.beginPath(); c.arc(12,12,5,.2,Math.PI-.2); c.stroke(); break;
        case "mic":
            c.beginPath(); c.arc(12,6,3,Math.PI,0); c.lineTo(15,12); c.arc(12,12,3,0,Math.PI); c.closePath(); c.stroke();
            c.beginPath(); c.arc(12,12,6,0,Math.PI); c.stroke();
            line(c, [[12,18],[12,22]]); line(c, [[8,22],[16,22]]); break;
        case "phone":
            line(c, [[6,3],[3,6],[4,11],[8,17],[14,21],[19,21],[22,18],[17,14],[14,17],[7,10],[10,7],[6,3]],true); break;
        case "settings":
            const points = [];
            for (let i=0; i<32; i++) { const a=i*Math.PI/16; const r=i%4<2?10:8; points.push([12+r*Math.cos(a),12+r*Math.sin(a)]); }
            line(c,points,true); circle(c,12,12,3); break;
        case "requests":
            line(c, [[3,4],[21,4],[21,17],[10,17],[5,21],[5,17],[3,17]],true);
            line(c, [[7,9],[17,9]]); line(c, [[7,13],[13,13]]); break;
        case "copy":
            c.strokeRect(8,8,12,13); line(c, [[15,5],[15,3],[3,3],[3,16],[5,16]]); break;
        case "send":
            line(c, [[3,3],[22,12],[3,21],[6,12],[3,3]],true); line(c,[[6,12],[15,12]]); break;
        case "play": line(c,[[7,4],[20,12],[7,20]],true); break;
        case "save":
            line(c, [[4,3],[17,3],[21,7],[21,21],[3,21],[3,3]],true);
            c.strokeRect(8,3,8,6); c.strokeRect(7,14,10,7); break;
        }
        c.restore();
    }
}
