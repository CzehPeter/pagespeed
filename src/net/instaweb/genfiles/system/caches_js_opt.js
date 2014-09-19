(function(){var k,l=l||{},m=this,aa=function(a,b){var c=a.split("."),d=m;c[0]in d||!d.execScript||d.execScript("var "+c[0]);for(var e;c.length&&(e=c.shift());)c.length||void 0===b?d[e]?d=d[e]:d=d[e]={}:d[e]=b},ba=function(a){a=a.split(".");for(var b=m,c;c=a.shift();)if(null!=b[c])b=b[c];else return null;return b},ca=function(){},n=function(a){var b=typeof a;if("object"==b)if(a){if(a instanceof Array)return"array";if(a instanceof Object)return b;var c=Object.prototype.toString.call(a);if("[object Window]"==c)return"object";
if("[object Array]"==c||"number"==typeof a.length&&"undefined"!=typeof a.splice&&"undefined"!=typeof a.propertyIsEnumerable&&!a.propertyIsEnumerable("splice"))return"array";if("[object Function]"==c||"undefined"!=typeof a.call&&"undefined"!=typeof a.propertyIsEnumerable&&!a.propertyIsEnumerable("call"))return"function"}else return"null";else if("function"==b&&"undefined"==typeof a.call)return"object";return b},da=function(a){var b=n(a);return"array"==b||"object"==b&&"number"==typeof a.length},p=function(a){return"string"==
typeof a},q=function(a){return"function"==n(a)},ea=function(a,b,c){return a.call.apply(a.bind,arguments)},fa=function(a,b,c){if(!a)throw Error();if(2<arguments.length){var d=Array.prototype.slice.call(arguments,2);return function(){var c=Array.prototype.slice.call(arguments);Array.prototype.unshift.apply(c,d);return a.apply(b,c)}}return function(){return a.apply(b,arguments)}},r=function(a,b,c){r=Function.prototype.bind&&-1!=Function.prototype.bind.toString().indexOf("native code")?ea:fa;return r.apply(null,
arguments)},ga=Date.now||function(){return+new Date},s=function(a,b){function c(){}c.prototype=b.prototype;a.O=b.prototype;a.prototype=new c;a.X=function(a,c,f){return b.prototype[c].apply(a,Array.prototype.slice.call(arguments,2))}};Function.prototype.bind=Function.prototype.bind||function(a,b){if(1<arguments.length){var c=Array.prototype.slice.call(arguments,1);c.unshift(this,a);return r.apply(null,c)}return r(this,a)};var t=function(a){if(Error.captureStackTrace)Error.captureStackTrace(this,t);else{var b=Error().stack;b&&(this.stack=b)}a&&(this.message=String(a))};s(t,Error);t.prototype.name="CustomError";var ha=function(a,b){for(var c=a.split("%s"),d="",e=Array.prototype.slice.call(arguments,1);e.length&&1<c.length;)d+=c.shift()+e.shift();return d+c.join("%s")},ia=String.prototype.trim?function(a){return a.trim()}:function(a){return a.replace(/^[\s\xa0]+|[\s\xa0]+$/g,"")},qa=function(a){if(!ja.test(a))return a;-1!=a.indexOf("&")&&(a=a.replace(ka,"&amp;"));-1!=a.indexOf("<")&&(a=a.replace(la,"&lt;"));-1!=a.indexOf(">")&&(a=a.replace(ma,"&gt;"));-1!=a.indexOf('"')&&(a=a.replace(na,"&quot;"));-1!=a.indexOf("'")&&
(a=a.replace(oa,"&#39;"));-1!=a.indexOf("\x00")&&(a=a.replace(pa,"&#0;"));return a},ka=/&/g,la=/</g,ma=/>/g,na=/"/g,oa=/'/g,pa=/\x00/g,ja=/[\x00&<>"']/,ra=function(a,b){return a<b?-1:a>b?1:0};var u=function(a,b){b.unshift(a);t.call(this,ha.apply(null,b));b.shift()};s(u,t);u.prototype.name="AssertionError";var v=function(a,b,c){if(!a){var d="Assertion failed";if(b)var d=d+(": "+b),e=Array.prototype.slice.call(arguments,2);throw new u(""+d,e||[]);}},sa=function(a,b){throw new u("Failure"+(a?": "+a:""),Array.prototype.slice.call(arguments,1));};var ta=function(a){ta[" "](a);return a};ta[" "]=ca;var w=Array.prototype,ua=w.indexOf?function(a,b,c){v(null!=a.length);return w.indexOf.call(a,b,c)}:function(a,b,c){c=null==c?0:0>c?Math.max(0,a.length+c):c;if(p(a))return p(b)&&1==b.length?a.indexOf(b,c):-1;for(;c<a.length;c++)if(c in a&&a[c]===b)return c;return-1},va=w.forEach?function(a,b,c){v(null!=a.length);w.forEach.call(a,b,c)}:function(a,b,c){for(var d=a.length,e=p(a)?a.split(""):a,f=0;f<d;f++)f in e&&b.call(c,e[f],f,a)},xa=function(a){var b;t:{b=wa;for(var c=a.length,d=p(a)?a.split(""):a,
e=0;e<c;e++)if(e in d&&b.call(void 0,d[e],e,a)){b=e;break t}b=-1}return 0>b?null:p(a)?a.charAt(b):a[b]};var ya=function(a){var b=[],c=0,d;for(d in a)b[c++]=a[d];return b},za=function(a){var b=[],c=0,d;for(d in a)b[c++]=d;return b},Aa="constructor hasOwnProperty isPrototypeOf propertyIsEnumerable toLocaleString toString valueOf".split(" "),Ba=function(a,b){for(var c,d,e=1;e<arguments.length;e++){d=arguments[e];for(c in d)a[c]=d[c];for(var f=0;f<Aa.length;f++)c=Aa[f],Object.prototype.hasOwnProperty.call(d,c)&&(a[c]=d[c])}};var z;t:{var Ca=m.navigator;if(Ca){var Da=Ca.userAgent;if(Da){z=Da;break t}}z=""}var A=function(a){return-1!=z.indexOf(a)};var Ea=A("Opera")||A("OPR"),B=A("Trident")||A("MSIE"),C=A("Gecko")&&-1==z.toLowerCase().indexOf("webkit")&&!(A("Trident")||A("MSIE")),D=-1!=z.toLowerCase().indexOf("webkit"),Fa=function(){var a=m.document;return a?a.documentMode:void 0},Ha=function(){var a="",b;if(Ea&&m.opera)return a=m.opera.version,q(a)?a():a;C?b=/rv\:([^\);]+)(\)|;)/:B?b=/\b(?:MSIE|rv)[: ]([^\);]+)(\)|;)/:D&&(b=/WebKit\/(\S+)/);b&&(a=(a=b.exec(z))?a[1]:"");return B&&(b=Fa(),b>parseFloat(a))?String(b):a}(),Ia={},E=function(a){var b;
if(!(b=Ia[a])){b=0;for(var c=ia(String(Ha)).split("."),d=ia(String(a)).split("."),e=Math.max(c.length,d.length),f=0;0==b&&f<e;f++){var g=c[f]||"",h=d[f]||"",x=RegExp("(\\d*)(\\D*)","g"),Ga=RegExp("(\\d*)(\\D*)","g");do{var G=x.exec(g)||["","",""],y=Ga.exec(h)||["","",""];if(0==G[0].length&&0==y[0].length)break;b=ra(0==G[1].length?0:parseInt(G[1],10),0==y[1].length?0:parseInt(y[1],10))||ra(0==G[2].length,0==y[2].length)||ra(G[2],y[2])}while(0==b)}b=Ia[a]=0<=b}return b},Ja=m.document,Ka=Ja&&B?Fa()||
("CSS1Compat"==Ja.compatMode?parseInt(Ha,10):5):void 0;var La;(La=!B)||(La=B&&9<=Ka);var Ma=La,Na=B&&!E("9");!D||E("528");C&&E("1.9b")||B&&E("8")||Ea&&E("9.5")||D&&E("528");C&&!E("8")||B&&E("9");var Oa=function(){this.w=this.w;this.Q=this.Q};Oa.prototype.w=!1;var F=function(a,b){this.type=a;this.a=this.b=b};F.prototype.d=function(){};C&&E(17);var H=function(a,b){F.call(this,a?a.type:"");this.e=this.a=this.b=null;if(a){this.type=a.type;this.b=a.target||a.srcElement;this.a=b;var c=a.relatedTarget;if(c&&C)try{ta(c.nodeName)}catch(d){}this.e=a;a.defaultPrevented&&this.d()}};s(H,F);H.prototype.d=function(){H.O.d.call(this);var a=this.e;if(a.preventDefault)a.preventDefault();else if(a.returnValue=!1,Na)try{if(a.ctrlKey||112<=a.keyCode&&123>=a.keyCode)a.keyCode=-1}catch(b){}};var I="closure_listenable_"+(1E6*Math.random()|0),Pa=0;var Qa=function(a,b,c,d,e){this.g=a;this.proxy=null;this.src=b;this.type=c;this.q=!!d;this.s=e;++Pa;this.removed=this.r=!1},Ra=function(a){a.removed=!0;a.g=null;a.proxy=null;a.src=null;a.s=null};var J=function(a){this.src=a;this.a={};this.b=0},Ta=function(a,b,c,d,e){var f=b.toString();b=a.a[f];b||(b=a.a[f]=[],a.b++);var g=Sa(b,c,d,e);-1<g?(a=b[g],a.r=!1):(a=new Qa(c,a.src,f,!!d,e),a.r=!1,b.push(a));return a};J.prototype.remove=function(a,b,c,d){a=a.toString();if(!(a in this.a))return!1;var e=this.a[a];b=Sa(e,b,c,d);return-1<b?(Ra(e[b]),v(null!=e.length),w.splice.call(e,b,1),0==e.length&&(delete this.a[a],this.b--),!0):!1};
var Ua=function(a,b){var c=b.type;if(c in a.a){var d=a.a[c],e=ua(d,b),f;if(f=0<=e)v(null!=d.length),w.splice.call(d,e,1);f&&(Ra(b),0==a.a[c].length&&(delete a.a[c],a.b--))}},Sa=function(a,b,c,d){for(var e=0;e<a.length;++e){var f=a[e];if(!f.removed&&f.g==b&&f.q==!!c&&f.s==d)return e}return-1};var Va="closure_lm_"+(1E6*Math.random()|0),Wa={},Xa=0,K=function(a,b,c,d,e){if("array"==n(b))for(var f=0;f<b.length;f++)K(a,b[f],c,d,e);else if(c=Ya(c),a&&a[I])a.listen(b,c,d,e);else{if(!b)throw Error("Invalid event type");var f=!!d,g=L(a);g||(a[Va]=g=new J(a));c=Ta(g,b,c,d,e);c.proxy||(d=Za(),c.proxy=d,d.src=a,d.g=c,a.addEventListener?a.addEventListener(b.toString(),d,f):a.attachEvent($a(b.toString()),d),Xa++)}},Za=function(){var a=ab,b=Ma?function(c){return a.call(b.src,b.g,c)}:function(c){c=a.call(b.src,
b.g,c);if(!c)return c};return b},bb=function(a,b,c,d,e){if("array"==n(b))for(var f=0;f<b.length;f++)bb(a,b[f],c,d,e);else(c=Ya(c),a&&a[I])?a.h.remove(String(b),c,d,e):a&&(a=L(a))&&(b=a.a[b.toString()],a=-1,b&&(a=Sa(b,c,!!d,e)),(c=-1<a?b[a]:null)&&cb(c))},cb=function(a){if("number"!=typeof a&&a&&!a.removed){var b=a.src;if(b&&b[I])Ua(b.h,a);else{var c=a.type,d=a.proxy;b.removeEventListener?b.removeEventListener(c,d,a.q):b.detachEvent&&b.detachEvent($a(c),d);Xa--;(c=L(b))?(Ua(c,a),0==c.b&&(c.src=null,
b[Va]=null)):Ra(a)}}},$a=function(a){return a in Wa?Wa[a]:Wa[a]="on"+a},eb=function(a,b,c,d){var e=1;if(a=L(a))if(b=a.a[b.toString()])for(b=b.concat(),a=0;a<b.length;a++){var f=b[a];f&&f.q==c&&!f.removed&&(e&=!1!==db(f,d))}return Boolean(e)},db=function(a,b){var c=a.g,d=a.s||a.src;a.r&&cb(a);return c.call(d,b)},ab=function(a,b){if(a.removed)return!0;if(!Ma){var c=b||ba("window.event"),d=new H(c,this),e=!0;if(!(0>c.keyCode||void 0!=c.returnValue)){t:{var f=!1;if(0==c.keyCode)try{c.keyCode=-1;break t}catch(g){f=
!0}if(f||void 0==c.returnValue)c.returnValue=!0}c=[];for(f=d.a;f;f=f.parentNode)c.push(f);for(var f=a.type,h=c.length-1;0<=h;h--)d.a=c[h],e&=eb(c[h],f,!0,d);for(h=0;h<c.length;h++)d.a=c[h],e&=eb(c[h],f,!1,d)}return e}return db(a,new H(b,this))},L=function(a){a=a[Va];return a instanceof J?a:null},fb="__closure_events_fn_"+(1E9*Math.random()>>>0),Ya=function(a){v(a,"Listener can not be null.");if(q(a))return a;v(a.handleEvent,"An object listener must have handleEvent method.");a[fb]||(a[fb]=function(b){return a.handleEvent(b)});
return a[fb]};var M=function(){Oa.call(this);this.h=new J(this);this.J=this};s(M,Oa);M.prototype[I]=!0;M.prototype.addEventListener=function(a,b,c,d){K(this,a,b,c,d)};M.prototype.removeEventListener=function(a,b,c,d){bb(this,a,b,c,d)};var N=function(a,b){gb(a);var c=a.J,d=b,e=d.type||d;if(p(d))d=new F(d,c);else if(d instanceof F)d.b=d.b||c;else{var f=d,d=new F(e,c);Ba(d,f)}c=d.a=c;hb(c,e,!0,d);hb(c,e,!1,d)};M.prototype.listen=function(a,b,c,d){gb(this);return Ta(this.h,String(a),b,c,d)};
var hb=function(a,b,c,d){if(b=a.h.a[String(b)]){b=b.concat();for(var e=!0,f=0;f<b.length;++f){var g=b[f];if(g&&!g.removed&&g.q==c){var h=g.g,x=g.s||g.src;g.r&&Ua(a.h,g);e=!1!==h.call(x,d)&&e}}}},gb=function(a){v(a.h,"Event target is not initialized. Did you call the superclass (goog.events.EventTarget) constructor?")};var ib="StopIteration"in m?m.StopIteration:Error("StopIteration"),jb=function(){};jb.prototype.next=function(){throw ib;};jb.prototype.V=function(){return this};var O=function(a,b){this.b={};this.a=[];this.e=this.d=0;var c=arguments.length;if(1<c){if(c%2)throw Error("Uneven number of arguments");for(var d=0;d<c;d+=2)this.set(arguments[d],arguments[d+1])}else if(a){a instanceof O?(c=a.i(),d=a.p()):(c=za(a),d=ya(a));for(var e=0;e<c.length;e++)this.set(c[e],d[e])}};O.prototype.p=function(){P(this);for(var a=[],b=0;b<this.a.length;b++)a.push(this.b[this.a[b]]);return a};O.prototype.i=function(){P(this);return this.a.concat()};
O.prototype.remove=function(a){return Object.prototype.hasOwnProperty.call(this.b,a)?(delete this.b[a],this.d--,this.e++,this.a.length>2*this.d&&P(this),!0):!1};var P=function(a){if(a.d!=a.a.length){for(var b=0,c=0;b<a.a.length;){var d=a.a[b];Object.prototype.hasOwnProperty.call(a.b,d)&&(a.a[c++]=d);b++}a.a.length=c}if(a.d!=a.a.length){for(var e={},c=b=0;b<a.a.length;)d=a.a[b],Object.prototype.hasOwnProperty.call(e,d)||(a.a[c++]=d,e[d]=1),b++;a.a.length=c}};k=O.prototype;
k.get=function(a,b){return Object.prototype.hasOwnProperty.call(this.b,a)?this.b[a]:b};k.set=function(a,b){Object.prototype.hasOwnProperty.call(this.b,a)||(this.d++,this.a.push(a),this.e++);this.b[a]=b};k.forEach=function(a,b){for(var c=this.i(),d=0;d<c.length;d++){var e=c[d],f=this.get(e);a.call(b,f,e,this)}};k.clone=function(){return new O(this)};
k.V=function(a){P(this);var b=0,c=this.a,d=this.b,e=this.e,f=this,g=new jb;g.next=function(){for(;;){if(e!=f.e)throw Error("The map has changed since the iterator was created");if(b>=c.length)throw ib;var g=c[b++];return a?g:d[g]}};return g};var kb=function(a){if("function"==typeof a.p)return a.p();if(p(a))return a.split("");if(da(a)){for(var b=[],c=a.length,d=0;d<c;d++)b.push(a[d]);return b}return ya(a)},lb=function(a,b){if("function"==typeof a.forEach)a.forEach(b,void 0);else if(da(a)||p(a))va(a,b,void 0);else{var c;if("function"==typeof a.i)c=a.i();else if("function"!=typeof a.p)if(da(a)||p(a)){c=[];for(var d=a.length,e=0;e<d;e++)c.push(e)}else c=za(a);else c=void 0;for(var d=kb(a),e=d.length,f=0;f<e;f++)b.call(void 0,d[f],c&&c[f],
a)}};var nb=function(a){var b;b||(b=mb(a||arguments.callee.caller,[]));return b},mb=function(a,b){var c=[];if(0<=ua(b,a))c.push("[...circular reference...]");else if(a&&50>b.length){c.push(ob(a)+"(");for(var d=a.arguments,e=0;d&&e<d.length;e++){0<e&&c.push(", ");var f;f=d[e];switch(typeof f){case "object":f=f?"object":"null";break;case "string":break;case "number":f=String(f);break;case "boolean":f=f?"true":"false";break;case "function":f=(f=ob(f))?f:"[fn]";break;default:f=typeof f}40<f.length&&(f=f.substr(0,
40)+"...");c.push(f)}b.push(a);c.push(")\n");try{c.push(mb(a.caller,b))}catch(g){c.push("[exception trying to get caller]\n")}}else a?c.push("[...long stack...]"):c.push("[end]");return c.join("")},ob=function(a){if(Q[a])return Q[a];a=String(a);if(!Q[a]){var b=/function ([^\(]+)/.exec(a);Q[a]=b?b[1]:"[Anonymous]"}return Q[a]},Q={};var R=function(a,b,c,d,e){"number"==typeof e||pb++;d||ga();this.d=b;delete this.b;delete this.a};R.prototype.b=null;R.prototype.a=null;var pb=0;R.prototype.getMessage=function(){return this.d};var S=function(){this.b=this.d=this.a=null},T=function(a,b){this.name=a;this.value=b};T.prototype.toString=function(){return this.name};var qb=new T("SEVERE",1E3),rb=new T("CONFIG",700),sb=new T("FINE",500);S.prototype.getChildren=function(){this.b||(this.b={});return this.b};var tb=function(a){if(a.d)return a.d;if(a.a)return tb(a.a);sa("Root logger has no level set.");return null};
S.prototype.log=function(a,b,c){if(a.value>=tb(this).value)for(q(b)&&(b=b()),a="log:"+this.e(0,b,c,S.prototype.log).getMessage(),m.console&&(m.console.timeStamp?m.console.timeStamp(a):m.console.markTimeline&&m.console.markTimeline(a)),m.msWriteProfilerMark&&m.msWriteProfilerMark(a),a=this;a;)a=a.a};
S.prototype.e=function(a,b,c,d){a=new R(0,String(b));if(c){a.b=c;var e;d=d||S.prototype.e;try{var f;var g=ba("window.location.href");if(p(c))f={message:c,name:"Unknown error",lineNumber:"Not available",fileName:g,stack:"Not available"};else{var h,x;b=!1;try{h=c.lineNumber||c.W||"Not available"}catch(Ga){h="Not available",b=!0}try{x=c.fileName||c.filename||c.sourceURL||m.$googDebugFname||g}catch(G){x="Not available",b=!0}f=!b&&c.lineNumber&&c.fileName&&c.stack&&c.message&&c.name?c:{message:c.message||
"Not available",name:c.name||"UnknownError",lineNumber:h,fileName:x,stack:c.stack||"Not available"}}e="Message: "+qa(f.message)+'\nUrl: <a href="view-source:'+f.fileName+'" target="_new">'+f.fileName+"</a>\nLine: "+f.lineNumber+"\n\nBrowser stack:\n"+qa(f.stack+"-> ")+"[end]\n\nJS stack traversal:\n"+qa(nb(d)+"-> ")}catch(y){e="Exception trying to expose exception! You win, we lose. "+y}a.a=e}return a};
var ub={},U=null,vb=function(a){U||(U=new S,ub[""]=U,U.d=rb);var b;if(!(b=ub[a])){b=new S;var c=a.lastIndexOf("."),d=a.substr(c+1),c=vb(a.substr(0,c));c.getChildren()[d]=b;b.a=c;ub[a]=b}return b};var V=function(a,b){a&&a.log(sb,b,void 0)};var wb=function(a,b,c){if(q(a))c&&(a=r(a,c));else if(a&&"function"==typeof a.handleEvent)a=r(a.handleEvent,a);else throw Error("Invalid listener argument");return 2147483647<b?-1:m.setTimeout(a,b||0)};var xb=/^(?:([^:/?#.]+):)?(?:\/\/(?:([^/?#]*)@)?([^/#?]*?)(?::([0-9]+))?(?=[/#?]|$))?([^?#]+)?(?:\?([^#]*))?(?:#(.*))?$/,yb=D,zb=function(a,b){if(yb){yb=!1;var c=m.location;if(c){var d=c.href;if(d&&(d=(d=zb(3,d))?decodeURI(d):d)&&d!=c.hostname)throw yb=!0,Error();}}return b.match(xb)[a]||null};var Ab=function(){};Ab.prototype.a=null;var Cb=function(a){var b;(b=a.a)||(b={},Bb(a)&&(b[0]=!0,b[1]=!0),b=a.a=b);return b};var Db,Eb=function(){};s(Eb,Ab);var Fb=function(a){return(a=Bb(a))?new ActiveXObject(a):new XMLHttpRequest},Bb=function(a){if(!a.b&&"undefined"==typeof XMLHttpRequest&&"undefined"!=typeof ActiveXObject){for(var b=["MSXML2.XMLHTTP.6.0","MSXML2.XMLHTTP.3.0","MSXML2.XMLHTTP","Microsoft.XMLHTTP"],c=0;c<b.length;c++){var d=b[c];try{return new ActiveXObject(d),a.b=d}catch(e){}}throw Error("Could not create ActiveXObject. ActiveX might be disabled, or MSXML might not be installed");}return a.b};Db=new Eb;var W=function(a){M.call(this);this.G=new O;this.o=a||null;this.b=!1;this.k=this.c=null;this.a=this.A=this.n="";this.d=this.t=this.j=this.u=!1;this.e=0;this.l=null;this.B="";this.m=this.H=!1};s(W,M);var Gb=W.prototype,Hb=vb("goog.net.XhrIo");Gb.f=Hb;var Ib=/^https?$/i,Jb=["POST","PUT"];
W.prototype.send=function(a,b,c,d){if(this.c)throw Error("[goog.net.XhrIo] Object is active with another request="+this.n+"; newUri="+a);b=b?b.toUpperCase():"GET";this.n=a;this.a="";this.A=b;this.u=!1;this.b=!0;this.c=this.o?Fb(this.o):Fb(Db);this.k=this.o?Cb(this.o):Cb(Db);this.c.onreadystatechange=r(this.C,this);try{V(this.f,X(this,"Opening Xhr")),this.t=!0,this.c.open(b,String(a),!0),this.t=!1}catch(e){V(this.f,X(this,"Error opening Xhr: "+e.message));Kb(this,e);return}a=c||"";var f=this.G.clone();
d&&lb(d,function(a,b){f.set(b,a)});d=xa(f.i());c=m.FormData&&a instanceof m.FormData;!(0<=ua(Jb,b))||d||c||f.set("Content-Type","application/x-www-form-urlencoded;charset=utf-8");f.forEach(function(a,b){this.c.setRequestHeader(b,a)},this);this.B&&(this.c.responseType=this.B);"withCredentials"in this.c&&(this.c.withCredentials=this.H);try{Lb(this),0<this.e&&(this.m=Mb(this.c),V(this.f,X(this,"Will abort after "+this.e+"ms if incomplete, xhr2 "+this.m)),this.m?(this.c.timeout=this.e,this.c.ontimeout=
r(this.D,this)):this.l=wb(this.D,this.e,this)),V(this.f,X(this,"Sending request")),this.j=!0,this.c.send(a),this.j=!1}catch(g){V(this.f,X(this,"Send error: "+g.message)),Kb(this,g)}};var Mb=function(a){return B&&E(9)&&"number"==typeof a.timeout&&void 0!==a.ontimeout},wa=function(a){return"content-type"==a.toLowerCase()};
W.prototype.D=function(){"undefined"!=typeof l&&this.c&&(this.a="Timed out after "+this.e+"ms, aborting",V(this.f,X(this,this.a)),N(this,"timeout"),this.c&&this.b&&(V(this.f,X(this,"Aborting")),this.b=!1,this.d=!0,this.c.abort(),this.d=!1,N(this,"complete"),N(this,"abort"),Nb(this)))};var Kb=function(a,b){a.b=!1;a.c&&(a.d=!0,a.c.abort(),a.d=!1);a.a=b;Ob(a);Nb(a)},Ob=function(a){a.u||(a.u=!0,N(a,"complete"),N(a,"error"))};W.prototype.C=function(){this.w||(this.t||this.j||this.d?Pb(this):this.P())};
W.prototype.P=function(){Pb(this)};
var Pb=function(a){if(a.b&&"undefined"!=typeof l)if(a.k[1]&&4==Y(a)&&2==Z(a))V(a.f,X(a,"Local request error detected and ignored"));else if(a.j&&4==Y(a))wb(a.C,0,a);else if(N(a,"readystatechange"),4==Y(a)){V(a.f,X(a,"Request complete"));a.b=!1;try{if(Qb(a))N(a,"complete"),N(a,"success");else{var b;try{b=2<Y(a)?a.c.statusText:""}catch(c){V(a.f,"Can not get status: "+c.message),b=""}a.a=b+" ["+Z(a)+"]";Ob(a)}}finally{Nb(a)}}},Nb=function(a){if(a.c){Lb(a);var b=a.c,c=a.k[0]?ca:null;a.c=null;a.k=null;
N(a,"ready");try{b.onreadystatechange=c}catch(d){(a=a.f)&&a.log(qb,"Problem encountered resetting onreadystatechange: "+d.message,void 0)}}},Lb=function(a){a.c&&a.m&&(a.c.ontimeout=null);"number"==typeof a.l&&(m.clearTimeout(a.l),a.l=null)},Qb=function(a){var b=Z(a),c;t:switch(b){case 200:case 201:case 202:case 204:case 206:case 304:case 1223:c=!0;break t;default:c=!1}if(!c){if(b=0===b)a=zb(1,String(a.n)),!a&&self.location&&(a=self.location.protocol,a=a.substr(0,a.length-1)),b=!Ib.test(a?a.toLowerCase():
"");c=b}return c},Y=function(a){return a.c?a.c.readyState:0},Z=function(a){try{return 2<Y(a)?a.c.status:-1}catch(b){return-1}},X=function(a,b){return b+" ["+a.A+" "+a.n+" "+Z(a)+"]"};var Rb=function(a){this.a=a||new W;this.b="";this.d=!1;a=document.createElement("table");a.id="nav-bar";a.className="pagespeed-sub-tabs";a.innerHTML='<tr><td><a id="show_metadata_mode" href="javascript:void(0);">Show Metadata Cache</a> - </td><td><a id="cache_struct_mode" href="javascript:void(0);">Show Cache Structure</a> - </td><td><a id="physical_cache_mode" href="javascript:void(0);">Physical Caches</a> - </td><td><a id="purge_cache_mode" href="javascript:void(0);">Purge Cache</a></td></tr>';
document.body.insertBefore(a,document.getElementById("show_metadata"));a=document.createElement("pre");a.id="metadata_result";a.className="pagespeed-caches-result";document.getElementById("show_metadata").appendChild(a);a=document.createElement("div");a.id="purge_result";a.className="pagespeed-caches-result";var b=document.getElementById("purge_cache");b.insertBefore(a,b.firstChild)};
aa("pagespeed.Caches.toggleDetail",function(a){var b=document.getElementById(a+"_summary"),c=document.getElementById(a+"_detail");document.getElementById(a+"_toggle").checked?(b.style.display="none",c.style.display="block"):(b.style.display="block",c.style.display="none")});var Sb={S:"show_metadata_mode",R:"cache_struct_mode",T:"physical_cache_mode",U:"purge_cache_mode"},$={S:"show_metadata",R:"cache_struct",T:"physical_cache",U:"purge_cache"};k=Rb.prototype;
k.F=function(){var a=location.hash.substr(1);if(""==a)this.show("show_metadata");else{var b;t:{for(b in $)if($[b]==a){b=!0;break t}b=!1}b&&this.show(a)}};k.show=function(a){for(var b in $){var c=$[b];document.getElementById(c).className=c==a?"":"pagespeed-hidden-offscreen"}c=document.getElementById(a+"_mode");for(b in Sb){var d=document.getElementById(Sb[b]);d.className=d==c?"pagespeed-underline-link":""}location.href=location.href.split("#")[0]+"#"+a};
k.M=function(){if(!this.a.c){var a=encodeURIComponent(document.getElementById("purge_text").value.trim());this.b="*"==a?"purge_all":"purge_text";this.a.send("?purge="+a)}};k.L=function(){this.a.c||(this.b="purge_all",this.a.send("?purge=*"))};k.v=function(){this.a.c||(this.b="purge_table",this.a.send("?new_set="))};
k.K=function(){if(!this.a.c){var a="?url="+encodeURIComponent(document.getElementById("metadata_text").value.trim())+"&user_agent="+encodeURIComponent(document.getElementById("user_agent").value.trim());this.b="metadata_result";this.a.send(a)}};k.I=function(){this.d=!this.d;this.v()};
k.N=function(){if(Qb(this.a)){var a;var b=this.a;try{a=b.c?b.c.responseText:""}catch(c){V(b.f,"Can not get responseText: "+c.message),a=""}if("metadata_result"==this.b)document.getElementById(this.b).textContent=a;else if("purge_table"==this.b){if(a=a.split("\n"),b=a.shift(),document.getElementById("purge_global").textContent="Everything before this time stamp is invalid: "+b.split("@")[1],b=document.getElementById("purge_table"),b.innerHTML="",0<a.length){b.appendChild(document.createElement("hr"));
var d=document.createElement("table");this.d&&a.reverse();for(var e=0;e<a.length;++e){var f=a[e].lastIndexOf("@"),g=a[e].substring(0,f),h=a[e].substring(f+1),f=d.insertRow(-1);f.insertCell(0).textContent=h;h=document.createElement("code");h.className="pagespeed-caches-purge-url";h.textContent=g;f.insertCell(1).appendChild(h)}e=d.createTHead().insertRow(0);g=e.insertCell(0);g.className="pagespeed-caches-date-column";1==a.length?g.textContent="Invalidation Time":(a=document.createElement("input"),a.setAttribute("type",
"checkbox"),a.id="sort",a.checked=this.d?!0:!1,a.title="Change sort order.",g.textContent=this.d?"Invalidation Time (Descending)":"Invalidation Time (Ascending)",g.appendChild(a),K(a,"change",r(this.I,this)));g=e.insertCell(1);g.textContent="URL";g.className="pagespeed-stats-url-column";b.appendChild(d)}}else window.setTimeout(r(this.v,this),0),b=document.getElementById("purge_result"),"Purge successful"==a&&"purge_text"==this.b?b.textContent="Added to Purge Set":-1!=a.indexOf("Purging not enabled")?
b.innerHTML=a:b.textContent=a}else a=this.a,console.log(p(a.a)?a.a:String(a.a))};
aa("pagespeed.Caches.Start",function(){K(window,"load",function(){var a=new Rb,b=document.createElement("table");b.innerHTML='URL: <input id="purge_text" type="text" name="purge" size="110"/><br><input id="purge_submit" type="button" value="Purge Individual URL"/><input id="purge_all" type="button" value="Purge Entire Cache"/>';var c=document.getElementById("purge_cache");c.insertBefore(b,c.firstChild);a.F();for(var d in $)K(document.getElementById(Sb[d]),"click",r(a.show,a,$[d]));K(window,"hashchange",
r(a.F,a));K(document.getElementById("purge_submit"),"click",r(a.M,a));K(document.getElementById("purge_all"),"click",r(a.L,a));K(document.getElementById("metadata_submit"),"click",r(a.K,a));K(a.a,"complete",r(a.N,a));K(document.getElementById("metadata_clear"),"click",location.reload.bind(location));a.v()})});})();
