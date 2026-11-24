#pragma once
static const char PAGE_HEAD[] = R"~(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>ESP32 Proxy</title>
<style>:root{color-scheme:light dark}*{box-sizing:border-box}body{font:16px/1.5 system-ui,sans-serif;max-width:34rem;margin:8vh auto;padding:0 16px}
input,button{font:inherit;width:100%;padding:.75rem;margin:.35rem 0;border:1px solid #888;border-radius:8px}button{background:#1463d8;color:#fff;border:0;cursor:pointer}
button.alt{background:none;color:inherit;border:1px solid #888}small{opacity:.75}code{overflow-wrap:anywhere}</style>)~";

static const char PAGE_PORTAL[] = R"~(<h1>Where to?</h1>
<form id=f><input id=u autofocus autocapitalize=none autocorrect=off spellcheck=false inputmode=url placeholder="example.com" aria-label=Address><button>Go</button></form>
<form method=post action=/release><button class=alt>Keep me connected and use my own browser</button></form>
<p><small>This ESP32 is the proxy: it fetches every page itself. Addresses ending in <b>.esp32</b> exist only on this Wi-Fi. Search words go to DuckDuckGo.</small></p><p><small>In your own browser, come back to <b>http://proxy.esp32/</b>. This proxy opens HTTPS for you, so it can see everything you view or type, and this Wi-Fi is open, so people nearby can too. Don't sign in or enter payment details.</small></p>
<p><a id=h href="http://192.168.4.1/">&#8592; All apps</a></p>
<script>try{var q=new URLSearchParams(location.search).get('home');if(/^http:\/\/[\w.:-]+$/.test(q||''))localStorage.home=q;if(localStorage.home)h.href=localStorage.home+'/'}catch(e){}
f.onsubmit=function(e){e.preventDefault();var v=u.value.trim();if(!v)return;
if(!/^https?:\/\//i.test(v)){if(/\s/.test(v)||!/^[^\/]+\.[^\/]+/.test(v))v='https://html.duckduckgo.com/html/?q='+encodeURIComponent(v);else v='https://'+v}
try{var x=new URL(v)}catch(e){alert('That address is not valid');return}
var s=x.protocol=='https:',h=x.hostname;if(h.slice(-6)!='.esp32')h+=(s?(x.port?'.p'+x.port:''):'.h'+x.port)+'.esp32';
location.href='http://'+h+x.pathname+x.search+x.hash}</script>)~";

static const char PAGE_SETUP[] = R"~(<h1>Connect the proxy to the internet</h1>
<form method=post action=/setup><input name=s placeholder="Your Wi-Fi name (2.4 GHz)" maxlength=32 required><input name=p type=password placeholder="Wi-Fi password" maxlength=63><button>Save and restart</button></form>
<p><small>Asked once. To change it later, send <code>setwifi=Name;password</code> over USB serial (115200), or hold BOOT for 5 seconds.</small></p>)~";

// Runs before the page's own scripts. Catches URLs built at runtime, which the static rewriter cannot see.
static const char HOOK_JS[] = R"~((function(){if(window.__px)return;window.__px=1;
var L=location;
function M(u){if(u==null||u==='')return u;var s=String(u).trim();if(!/^(https?:|wss?:|\/\/)/i.test(s))return u;
try{var x=new URL(s.indexOf('//')?s:'https:'+s)}catch(e){return u}
var h=x.hostname;if(!h||/\.esp32$/.test(h)||h.indexOf('.')<0||/(^|\.)w3\.org$/.test(h))return u;
var sec=/^(https|wss):$/.test(x.protocol),ws=/^ws/.test(x.protocol);
h+=sec?(x.port?'.p'+x.port:''):'.h'+x.port;
return(ws?'ws':'http')+'://'+h+'.esp32'+x.pathname+x.search+x.hash}
function W(u){u=M(u);try{var x=new URL(u,L.href);if(x.protocol=='wss:'&&/\.esp32$/.test(x.hostname)){x.protocol='ws:';return x.href}}catch(e){}return u}
function S(v){return String(v).replace(/(^|\s|,)((?:https?:)?\/\/[^\s,]+)/g,function(_,a,b){return a+M(b)})}
function E(){return''}
function T(f){try{f()}catch(e){}}
// YouTube reloads itself to switch to dark, downloading a second 2.3 MB build. Report "light" to avoid that.
if(/(^|\.)youtube\.com\.esp32$/.test(L.hostname))T(function(){var m=window.matchMedia;window.matchMedia=function(q){return m.call(window,String(q).replace(/\(\s*prefers-color-scheme\s*:\s*dark\s*\)/gi,'(width<0px)'))}});
T(function(){var f=window.fetch;window.fetch=function(i,o){try{if(o&&o.integrity){o=Object.assign({},o);delete o.integrity}
if(window.Request&&i instanceof Request){var m=M(i.url);
if(i.method=='GET'||i.method=='HEAD')return f.call(window,m===i.url?i:new Request(m,i),o);
if(m!==i.url||i.duplex=='half'){var q=i,n={method:q.method,headers:q.headers,credentials:q.credentials,cache:q.cache,redirect:q.redirect,signal:q.signal,keepalive:q.keepalive,mode:q.mode=='navigate'?'cors':q.mode};
return q.arrayBuffer().then(function(b){n.body=b;return f.call(window,m,Object.assign(n,o))})}
return f.call(window,i,o)}
if(o&&window.ReadableStream&&o.body instanceof ReadableStream)return new Response(o.body).arrayBuffer().then(function(b){o=Object.assign({},o,{body:b});delete o.duplex;return f.call(window,M(i),o)});
return f.call(window,M(i),o)}catch(e){return Promise.reject(e)}}});
T(function(){var Q=window.Request;if(!Q)return;var P=new Proxy(Q,{construct:function(t,a,n){if(!(a[0] instanceof Q))a[0]=M(a[0]);if(a[1]&&a[1].integrity){a[1]=Object.assign({},a[1]);delete a[1].integrity}return Reflect.construct(t,a,n===P?t:n)}});window.Request=P});
T(function(){var o=XMLHttpRequest.prototype.open;XMLHttpRequest.prototype.open=function(){var a=[].slice.call(arguments);a[1]=M(a[1]);return o.apply(this,a)}});
['WebSocket','EventSource','Worker','SharedWorker'].forEach(function(k){T(function(){var C=window[k];if(!C)return;
var P=new Proxy(C,{construct:function(t,a,n){a[0]=k=='WebSocket'?W(a[0]):M(a[0]);return Reflect.construct(t,a,n===P?t:n)}});window[k]=P})});
T(function(){var b=navigator.sendBeacon;navigator.sendBeacon=function(u,d){return b.call(navigator,M(u),d)}});
T(function(){var w=window.open;window.open=function(u){var a=[].slice.call(arguments);if(a.length)a[0]=M(u);return w.apply(window,a)}});
['pushState','replaceState'].forEach(function(k){T(function(){var f=history[k];history[k]=function(s,t,u){return f.call(history,s,t,M(u))}})});
var R={srcset:S,integrity:E};
[['HTMLImageElement','src srcset'],['HTMLScriptElement','src integrity'],['HTMLLinkElement','href integrity'],['HTMLAnchorElement','href'],['HTMLIFrameElement','src'],['HTMLMediaElement','src'],['HTMLSourceElement','src srcset'],['HTMLFormElement','action'],['HTMLInputElement','src formAction'],['HTMLButtonElement','formAction'],['HTMLVideoElement','poster'],['HTMLObjectElement','data'],['HTMLEmbedElement','src'],['HTMLTrackElement','src'],['HTMLBaseElement','href']].forEach(function(p){
p[1].split(' ').forEach(function(k){T(function(){var P=window[p[0]].prototype,d=Object.getOwnPropertyDescriptor(P,k);if(!d||!d.set)return;
Object.defineProperty(P,k,{configurable:true,enumerable:d.enumerable,get:d.get,set:function(v){d.set.call(this,(R[k]||M)(v))}})})})});
var A={src:M,href:M,action:M,formaction:M,poster:M,data:M,srcset:S,imagesrcset:S,'xlink:href':M,integrity:E},sa=Element.prototype.setAttribute;
Element.prototype.setAttribute=function(k,v){k=String(k);var n=k.toLowerCase();return sa.call(this,k,Object.prototype.hasOwnProperty.call(A,n)?A[n](v):v)};
function C(t){return t.replace(/(url\(\s*["']?)((?:https?:)?\/\/[^"')\s]+)/g,function(_,a,b){return a+M(b)})}
T(function(){var f=CSSStyleSheet.prototype.insertRule;CSSStyleSheet.prototype.insertRule=function(t,i){return f.call(this,C(String(t)),i)}});
function fix(e){if(e.nodeType!=1)return;if(e.tagName=='STYLE'&&e.textContent.indexOf('//')>0){var q=e.textContent,z=C(q);if(z!==q)e.textContent=z}for(var k in A){if(k=='integrity'||!e.hasAttribute(k))continue;var v=e.getAttribute(k),m=A[k](v);if(m!==v)sa.call(e,k,m)}}
new MutationObserver(function(l){l.forEach(function(m){if(m.type=='attributes')return fix(m.target);m.addedNodes.forEach(function(n){fix(n);if(n.querySelectorAll)n.querySelectorAll('[src],[href],[action],[srcset],[poster],[data]').forEach(fix)})})}).observe(document,{childList:true,subtree:true,attributes:true,attributeFilter:['src','href','action','srcset','poster','data']});
addEventListener('click',function(e){var a=e.target&&e.target.closest&&e.target.closest('a[href]');if(a)fix(a)},true);
addEventListener('submit',function(e){fix(e.target)},true);
T(function(){navigation.addEventListener('navigate',function(e){var u=e.destination.url,m=M(u);if(m!==u&&e.cancelable){e.preventDefault();L.href=m}})});
T(function(){var d=Object.getOwnPropertyDescriptor(Document.prototype,'cookie');Object.defineProperty(document,'cookie',{configurable:true,
get:function(){return d.get.call(document).replace(/(^|;\s*)x(Host|Secure)-/g,'$1__$2-')},
set:function(v){var x=(L.hostname.match(/(\.[hp]\d*)?\.esp32$/)||['.esp32'])[0];d.set.call(document,String(v).replace(/^__(Host|Secure)-/,'x$1-').replace(/;\s*(secure|partitioned)(?=\s*(;|$))/gi,'').replace(/;\s*samesite\s*=[^;]*/gi,'').replace(/;\s*domain\s*=\s*\.?([^;\s]+)/gi,function(m,h){return'; domain='+(/\.esp32$/.test(h)?h:h+x)}))}})});
T(function(){if(!crypto.randomUUID)crypto.randomUUID=function(){var b=crypto.getRandomValues(new Uint8Array(16));b[6]=b[6]&15|64;b[8]=b[8]&63|128;var h=[].map.call(b,function(x){return(x+256).toString(16).slice(1)}).join('');return h.replace(/(.{8})(.{4})(.{4})(.{4})(.{12})/,'$1-$2-$3-$4-$5')}});
if(window===top)addEventListener('DOMContentLoaded',function(){var a=document.createElement('a');a.href='http://proxy.esp32/';a.textContent='⌂';a.title='Proxy address bar';
a.style.cssText='all:initial;position:fixed;left:8px;bottom:8px;z-index:2147483647;width:40px;height:40px;border-radius:50%;background:#1463d8;color:#fff;font:24px/40px sans-serif;text-align:center;cursor:pointer;opacity:.75';document.documentElement.appendChild(a)});
})();)~";
