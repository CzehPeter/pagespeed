(function(){var a=window,b="prototype",c="",g=",",h=":",k="div",l="height",m="id",n="pagespeed.detectReflow.preScriptHeights_ is not available";a.pagespeed=a.pagespeed||{};var o=a.pagespeed,p=function(){this.b={};this.d=c;this.e=!1};p[b].f=function(){return this.b};p[b].getPreScriptHeights=p[b].f;p[b].g=function(){return this.d};p[b].getReflowElementHeight=p[b].g;p[b].h=function(){return this.e};p[b].isJsDeferDone=p[b].h;o.a=new p;o.detectReflow=o.a;
o.i=function(){for(var f=document.getElementsByTagName(k),i=f.length,e=0;e<i;++e){var d=f[e];d.hasAttribute(m)&&void 0!=d.clientHeight&&(o.a.b[d.getAttribute(m)]=d.clientHeight)}};o.k=function(){!o.a.b&&console&&console.log(n);for(var f=document.getElementsByTagName(k),i=f.length,e=0;e<i;++e){var d=f[e];if(d.hasAttribute(m)){var j=d.getAttribute(m);void 0!=o.a.b[j]&&o.a.b[j]!=d.clientHeight&&(o.a.d=o.a.d+j+h+a.getComputedStyle(d,null).getPropertyValue(l)+g)}}};o.j=function(){o.a.e=!0};
"undefined"!=o.c&&("undefined"!=o.c.addBeforeDeferRunFunctions&&"undefined"!=o.c.addAfterDeferRunFunctions)&&(o.c.addBeforeDeferRunFunctions(o.i),o.c.addAfterDeferRunFunctions(o.j));})();