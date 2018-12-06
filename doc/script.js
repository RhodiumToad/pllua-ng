(function(){
    "use strict";
    var shiftWindow5 = function() { scrollBy(0, -0.05 * window.innerHeight) };
    window.addEventListener("DOMContentLoaded",(function(){ if (location.hash) shiftWindow5(); }));
    window.addEventListener("hashchange", shiftWindow5);
})();
