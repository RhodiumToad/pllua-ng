(function(){
    "use strict";
    /* adjust scroll positions */
    var shiftWindow5 = function() { scrollBy(0, -0.05 * window.innerHeight) };
    window.addEventListener("DOMContentLoaded",(function(){ if (location.hash) shiftWindow5(); }));
    window.addEventListener("hashchange", shiftWindow5);
    /* render the logo into a high-res favicon */
    window.addEventListener("DOMContentLoaded",(function(){
	var logosrc = (window.getComputedStyle(document.getElementById("logo"))
		       .getPropertyValue("background-image")
		       .match(/data:[^"")]*/)[0]);
	if (logosrc.length)
	{
	    var i = new Image(960,960);
	    var render1 = function(id,s) {
		var c = document.createElement("canvas");
		c.width = s;
		c.height = s;
		var cxt = c.getContext("2d");
		cxt.drawImage(i, 0, 0, s, s);
		var link = document.createElement("link");
		link.id = id;
		link.rel = "icon";
		link.type = "image/png";
		link.sizes = s+"x"+s;
		link.href = c.toDataURL();
		return link;
	    };
	    var render = function() {
		document.head.appendChild(render1("dyn-icon-192.png", 192));
	    };
	    i.src = logosrc;
	    if (i.complete) {
		render();
	    } else {
		i.onload = (function(){ if (i.complete) { render(); } });
	    }
	}
    }));
})();
