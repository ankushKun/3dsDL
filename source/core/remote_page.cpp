#include "remote.hpp"

// The page a phone or computer sees on the Receive screen's address: a box
// for links, which works without JavaScript, and a file browser for the SD
// card that talks to the JSON API in net/remote_server. Colours follow
// docs/DESIGN.md; the layout is Finder's: places on the left, the folder in
// the middle, the selection's details on the right.
//
// The console serves one connection at a time, so the script sends every
// request through one queue ("the lane"): what the person asked for goes
// first, uploads follow in order, and icons and status checks fill the gaps.

namespace remote {

namespace {

std::string escapeHtml(const std::string &value)
{
    std::string out;
    for (const char c : value) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

const char *const PageStart = R"PAGE(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#e9e1a8">
<link rel="icon" type="image/png" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAIAAAD8GO2jAAAABGdBTUEAALGPC/xhBQAAACBjSFJNAAB6JgAAgIQAAPoAAACA6AAAdTAAAOpgAAA6mAAAF3CculE8AAAARGVYSWZNTQAqAAAACAABh2kABAAAAAEAAAAaAAAAAAADoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEAAAAgAAAAAKyGYvMAAAHLaVRYdFhNTDpjb20uYWRvYmUueG1wAAAAAAA8eDp4bXBtZXRhIHhtbG5zOng9ImFkb2JlOm5zOm1ldGEvIiB4OnhtcHRrPSJYTVAgQ29yZSA2LjAuMCI+CiAgIDxyZGY6UkRGIHhtbG5zOnJkZj0iaHR0cDovL3d3dy53My5vcmcvMTk5OS8wMi8yMi1yZGYtc3ludGF4LW5zIyI+CiAgICAgIDxyZGY6RGVzY3JpcHRpb24gcmRmOmFib3V0PSIiCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIj4KICAgICAgICAgPGV4aWY6Q29sb3JTcGFjZT4xPC9leGlmOkNvbG9yU3BhY2U+CiAgICAgICAgIDxleGlmOlBpeGVsWERpbWVuc2lvbj4yNTY8L2V4aWY6UGl4ZWxYRGltZW5zaW9uPgogICAgICAgICA8ZXhpZjpQaXhlbFlEaW1lbnNpb24+MjU2PC9leGlmOlBpeGVsWURpbWVuc2lvbj4KICAgICAgPC9yZGY6RGVzY3JpcHRpb24+CiAgIDwvcmRmOlJERj4KPC94OnhtcG1ldGE+CuYattQAAAaYSURBVEgNtVZLj1zFFT6n7rsfM+72zBhkbAG2F4MdBRBIVpAiWHiRSNmBkggJWWILO9ggJK9gwY6fkLBIFLFBCkIIhIWExSPiYYVFMBo0YxsznnGPZ9zT3fdRD75T1T1j7Ci7lG5X1617zvedR51zL1vrmOn/N2JH5PDDAI1f/He62S5ERPA2yaAdTJxJhTuZQRBULJmB4wWl2DqBmA3/1G1w/SmZBvLkuOGH4uKEZwmCUwuh8gtVDxEDDvsRr3DzsWm6Y/dY0T0mm+xpAnv5T2W+ock1YsXU3Nxa7h9/TampDfvw0w35E1Q/FDiNmYw2P2bWur5cTrYFmCEARefYi1pLqiDO/FWQ2zWmgaIIzKBEQS6vddusLECilk2furI6WlltWyruEIEdjjrBGpkZXpRGV7IOkNN/3IFSLgQggGAhScZd1n2wzWcXOgcjRQYiPjw+wOIE85zAMYJi8ReBAPkQ5+S35wQ8U3bV0BJFS3uuKaygBJpW9yDEcWrB53X8DHS7TmpMNPJSmLjItar+xfoHZ2sh9kMMr1bi+gOz+5GWAIo6RiwmAR7YYqsf3n7ZY25uvZ9Wb8VJREhDGI6KIiL+6+D7oZ575eDh085KMoBQm/76tV6n90BL+dPv5X2IsJJICY1wePsjpoTJuqPXVpJD/c2s3yIVwXwv5oYbu4PJmSPHHgYvVGEtfmn7yOKxs0FfNkSUeKc2UBI9j44J65hp09InJY0sj4c3dq5/uNzfeah1JefGEt8o849+OvXsqT/c184qia/HBwIsk6QEPr8A3HZtgC0cUwKC7SNLb4z5i21T1U6pCOfGWcqpQaSg37i4pOhYs3PuvuxENy9NMDa44XE9WYgpjul0yydF1qil7yu7VtmWcZIeY5zWbHVlVaMyqzLsFaTX4vZLl6vPB+M8Ap5Yjf29Sxzz1/SYTmMkkiIaOTmssaI/9dTxDMlgBI2tvbR21Rnz6PHDeRwhQ2M79/aPt7aa4ZlD3Qp+BI49Jr+IsSvomDH8AsscDcG6XxXqxcVZQyCajOq/vXd+PKmefvGZfmfeK9BksfXy2jiy20/ec6BG0e5zCDDOloIv+1HyRNjJlJzdZB9c0KIsTbOsNG5YoetNRy9yNs0v3NiFbXfioMKcm+YAz3AjFjiH4CSAIxp7c2ZQdP6r724OUXH0l/c/2x2XYb8dca5415KWEhWOGZTPgSNUMnClmENawgLxjZwbo4WH0HmwfrdAYZR1c3TxQJGngaCluFB0y1JjhCBcASTAhkp2PhiMB6HdoFITchM5kVITYTy+/MCrz+Vff7f2xzOnp1toMIo7QsCVMTHDXAEJmQAB+OR9IFUgpS5tTTQZrweXkivFA8jI5rXBzqXL17Wx3U777x98MS6r350+ee9iP1cuJhqSKrVtx0AXBeGYLni/FwE9mA8iLHDaNxytTsyptoLwD1fWL1y8lOVplCYcxypN/zNq1nj34sRdt62E1agxrRwJkL7kbWdQgek2D6TfSZVhhlEZE9rAuXVzIiprR9X80frJ+yviynFNjGb9TmP0FllWieKa1bDRC2I7cuo9mM0x0gh74Rfs9KGSW0Ql87cDF63rGKwYsAvJ1wAgLFxNCpXoS9IZFQ0bhBOvmmB+mL3FULvjAiVAMh9KIOOkI+FddsuZQxIfKfg3BSGxLyyo5cT+fl69fg8j1Tdr0QMJOLAKmFhIDgCHPMvsTxFm6z1AZH5d8CuH1FsbVS9Pz/bp3JX6+aV8gfSbm/bPvXjOuiylJzp8OFUDf04F1wOGGc4iB+KypMafIdyAAMzwQDs6ktC9CZ9M3S12CUdLkZ2greLkorvKGaUbOMjEnVhtTyzMFy9mHIILi2E3LnR5tE4znT0BeEgqCCNXNISq71Ej4zI0dF+3vYhGWrr9fMRbGgSiqL192IUqrvBdJCgYUz8gpwh1AIqJx51jt4M3DXasXSv1ydj+qPnCTnO5tOcnSXuzvFirk9YYX/mIUrBdAEFw2608wGOECsHJPd+KVu8Oqq9H9KmJ9dXy8zIZjd0/Nuqrmi9uj61u8Kr4tqmXdPnb44cqmL+H7S3GHX9zc781+k0hRgy2G/3mT6PV2hqtI92kpsGH2RyZDlkcoYOJOpDEvTQ5kCVzWbLUabfzXMM6MfoXg7+6iwAiEIyZt8v63xsDsqaXJfNZ2k2TIk2yGN8Y+IJFQ0dE4bEkFo3Ov/zvggfhl1t3ezA1QTGjSiEjR0NC6Q8clrOwguB/D5z+nwFBOSw475JwzgAAAABJRU5ErkJggg=="><title>3dsDL</title><style>
:root{color-scheme:light;--ink:#4f493b;--soft:#665e47;--text:#141414;--blue:#1c93c2;--deep:#0f6f96;--recess:#d9d3a0;--dots:#b0a66e;--good:#3fa65a;--bad:#e0301a;--red:#b8240d;--warn:#e69614;--cream:#fffdf4;--card:linear-gradient(#fffdf4,#f4efd8);--line:#d6cc8f;--tag:#3f7a91;--pick:#fff3c4;--ring:#e2ad3a;--ease:cubic-bezier(.2,.8,.2,1);--spring:cubic-bezier(.2,.9,.3,1.25)}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html{background:#f0e694}
body{margin:0;font:15px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif;background:linear-gradient(#f2ebb2,#f0e694);color:var(--ink)}
[hidden]{display:none!important}
a{color:var(--deep)}
.i{width:18px;height:18px;flex:none;fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.i.solid{fill:currentColor;stroke:none}
button{font:inherit}
@keyframes rise{from{opacity:0;transform:translateY(8px)}}
@keyframes fadein{from{opacity:0}}
@keyframes pop{from{opacity:0;transform:translateY(10px) scale(.95)}}
@keyframes spin{to{transform:rotate(1turn)}}
@keyframes ring{0%{box-shadow:0 0 0 0 currentColor}70%,100%{box-shadow:0 0 0 7px transparent}}
@keyframes pulse{50%{opacity:.35}}
@keyframes shake{20%,60%{transform:translateX(-6px)}40%,80%{transform:translateX(6px)}}
@keyframes shimmer{from{background-position:200% 0}to{background-position:-200% 0}}
@keyframes stripes{to{background-position:18px 0,0 0}}
.app{display:flex;height:100vh;height:100dvh}
.content{flex:1;min-width:0;display:flex;flex-direction:column;overflow:hidden}
.view{flex:1;min-height:0;display:flex;flex-direction:column}
/* Sidebar */
.side{width:248px;flex:none;overflow-y:auto;display:flex;flex-direction:column;gap:18px;padding:18px 12px 16px;background:linear-gradient(#e9e1a8,#e2d99c);border-right:1px solid #cbc187}
.brand{display:flex;flex-direction:column;gap:9px;padding:0 6px}
.logo{display:flex;align-items:center;gap:10px;text-decoration:none}
.logo img{width:32px;height:32px;border-radius:9px;box-shadow:0 1px 3px #0003;transition:transform .3s var(--spring)}
.logo:hover img{transform:rotate(-8deg) scale(1.08)}
.logo b{font-size:19px;font-weight:750;color:var(--deep);letter-spacing:-.01em}
.ext{display:flex;gap:12px;padding-left:42px;font-size:12px;font-weight:600}
.ext a{display:inline-flex;align-items:center;gap:3px;color:var(--soft);text-decoration:none;transition:color .2s}
.ext a:hover{color:var(--deep)}
.ext .i{width:11px;height:11px}
.pill{display:inline-flex;align-self:flex-start;align-items:center;gap:7px;max-width:100%;padding:5px 11px 5px 9px;border-radius:999px;background:#fffdf0cc;border:1px solid var(--line);font-size:13px;font-weight:600;color:var(--ink);transition:background .3s,border-color .3s}
.pill span{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.dot{width:8px;height:8px;border-radius:50%;flex:none;background:var(--good)}
.pill[data-state=connecting] .dot{background:#aca27a;animation:pulse 1.2s infinite}
.pill[data-state=asking] .dot,.pill[data-state=choosing] .dot{background:var(--warn);color:#e6961466;animation:ring 1.4s infinite}
.pill[data-state=asking],.pill[data-state=choosing]{background:#fff3cf;border-color:#ebc56a}
.pill[data-state=installing] .dot,.pill[data-state=downloading] .dot,.pill[data-state=sending] .dot,.pill[data-state=busy] .dot{background:none;border:2px solid #9fd3ea;border-top-color:var(--blue);animation:spin .8s linear infinite}
.pill[data-state=offline]{background:#ffe5df;border-color:#f0a596;color:#9a1d0b}
.pill[data-state=offline] .dot{background:var(--bad)}
.group{display:flex;flex-direction:column;gap:2px}
.group h3{margin:0;padding:0 10px 4px;font-size:12px;font-weight:700;color:var(--soft)}
.nav a{display:flex;align-items:center;gap:10px;min-height:34px;padding:0 10px;border-radius:9px;text-decoration:none;font-size:14px;font-weight:550;color:var(--ink);transition:background .15s}
.nav a span{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.nav a .i{color:#7d7563}
.nav a .i.star{color:#c99a12}
.nav a:hover{background:#fffdf055}
.nav a.on{font-weight:700;color:var(--text);background:linear-gradient(#fffdf4,#f1ecd6);box-shadow:0 1px 2px #0002,inset 0 1px 0 #fff}
.nav a.on .i:not(.star){color:var(--deep)}
.badge{min-width:20px;height:20px;padding:0 6px;border-radius:10px;background:#d97a0a;color:#fff;font-size:12px;font-weight:700;display:grid;place-items:center;flex:none}
body.gated .places a{color:#9a9172;pointer-events:none}
body.gated .places .i{color:#b0a77f}
.storage{margin-top:auto;padding:12px 10px 0;border-top:1px dotted var(--dots);display:flex;flex-direction:column;gap:7px;font-size:13px}
.storage div:first-child{display:flex;justify-content:space-between;gap:8px}
.storage b{font-weight:650}
.storage span{color:var(--soft);font-variant-numeric:tabular-nums}
.meter{height:7px;border-radius:4px;background:#cfc795;box-shadow:inset 0 1px 2px #0002;overflow:hidden}
.meter i{display:block;height:100%;width:0;border-radius:4px;background:linear-gradient(90deg,#35aee0,#1683b0);transition:width .6s var(--ease)}
.meter.full i{background:linear-gradient(90deg,#ffb020,#e0301a)}
.drop{box-shadow:inset 0 0 0 2px var(--blue)!important;background:#dff1fa!important}
.mhead,.scrim{display:none}
/* Toolbars and buttons */
.bar{display:flex;flex-wrap:wrap;align-items:center;gap:10px;min-height:62px;padding:14px 18px;border-bottom:1px dotted var(--dots)}
.bar h1{margin:0 6px 0 4px;font-size:20px;font-weight:750;color:var(--text);letter-spacing:-.01em;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.grow{flex:1}
.brk{display:none}
.btn{display:inline-flex;align-items:center;justify-content:center;gap:7px;height:36px;padding:0 14px;border:1px solid #8f8659;border-radius:10px;font:600 14px system-ui,-apple-system,sans-serif;color:var(--ink);background:linear-gradient(#fefcf5,#e0dccb);cursor:pointer;white-space:nowrap;position:relative;text-decoration:none;transition:transform .12s var(--ease),background .2s;box-shadow:inset 0 1px 0 #fff8,0 1px 2px #0001}
.btn:hover{background:linear-gradient(#fffef9,#e8e4d4)}
.btn:active{transform:scale(.97)}
.btn.primary{color:#fff;border-color:#10668a;background:linear-gradient(#2fa3d4,#136f97);box-shadow:inset 0 1px 0 #fff5,0 2px 6px #1c93c244}
.btn.primary:hover{background:linear-gradient(#3aaedd,#1679a3)}
.btn.danger{color:#fff;border-color:#8f1c09;background:linear-gradient(#d9472b,#a82a12);box-shadow:inset 0 1px 0 #fff4,0 2px 6px #a82a1244}
.btn.red{color:var(--red)}
.btn.icon{width:38px;padding:0}
.btn.small{height:30px;padding:0 10px;font-size:13px;border-radius:8px}
.btn:disabled{opacity:.45;cursor:default;transform:none}
.btn.busy{color:transparent!important;pointer-events:none}
.btn.busy .i{opacity:0}
.btn.busy::after{content:"";position:absolute;inset:0;margin:auto;width:17px;height:17px;border-radius:50%;border:2.5px solid #8c846655;border-top-color:#756e60;animation:spin .7s linear infinite}
.btn.primary.busy::after,.btn.danger.busy::after{border-color:#fff6;border-top-color:#fff}
button:focus-visible,.btn:focus-within,a:focus-visible,.item:focus-visible{outline:3px solid #3cdc78;outline-offset:2px}
.seg{display:flex;padding:3px;border-radius:10px;background:var(--recess);box-shadow:inset 0 1px 2px #0002}
.seg button{width:38px;height:28px;border:0;border-radius:7px;background:none;color:#5d5646;display:grid;place-items:center;cursor:pointer}
.seg button[aria-pressed=true]{background:linear-gradient(#fefcf5,#e8e3cf);box-shadow:0 1px 3px #0003;color:var(--deep)}
.search{position:relative;display:flex;align-items:center;flex:0 1 220px;min-width:150px}
.search .i{position:absolute;left:11px;color:var(--soft);pointer-events:none}
input[type=search],input[type=text],textarea{width:100%;border:1px solid #b3aa83;border-radius:10px;background:var(--cream);color:var(--text);transition:border-color .2s,box-shadow .2s}
input[type=search]{height:36px;padding:0 12px 0 34px;font:14px system-ui,-apple-system,sans-serif}
input[type=text]{padding:10px 12px;font:15px system-ui,-apple-system,sans-serif}
textarea{padding:14px 16px;font:14px/1.6 ui-monospace,SFMono-Regular,Menlo,monospace;min-height:180px;resize:vertical}
input:focus,textarea:focus{outline:none;border-color:var(--blue);box-shadow:0 0 0 3px #1c93c233}
input[type=checkbox]{width:16px;height:16px;margin:0;accent-color:var(--blue);flex:none;cursor:pointer}
.check{display:flex;gap:8px;align-items:center;font-size:14px;cursor:pointer}
.spin{display:inline-block;flex:none;width:18px;height:18px;border-radius:50%;border:2.5px solid #cfc795;border-top-color:var(--blue);animation:spin .8s linear infinite;vertical-align:-4px}
.banner{display:flex;gap:10px;align-items:center;margin:12px 18px 0;background:#ffe5df;border:1px solid #f0a596;color:#9a1d0b;border-radius:12px;padding:10px 12px;font-size:14px;animation:rise .3s var(--ease)}
.banner .spin{border-color:#f0a596;border-top-color:#9a1d0b}
.ccard{display:flex;gap:12px;align-items:center;margin:14px 18px 0;background:linear-gradient(#fff8dc,#fff0b8);border:1px solid #ebc56a;border-radius:14px;padding:12px 14px;animation:rise .35s var(--ease)}
.ccard .art{width:40px;height:40px;border-radius:50%;display:grid;place-items:center;background:#fff;color:var(--warn);flex:none;box-shadow:0 1px 3px #0002}
.ccard b{display:block;color:var(--text)}
.ccard span{font-size:13px}
/* Links and Transfers */
.pad{flex:1;min-height:0;overflow-y:auto;padding:18px}
.pad>*{max-width:880px}
.note{border-radius:12px;padding:10px 12px;margin-bottom:14px;color:#fff;animation:rise .35s var(--ease)}
.note.good{background:var(--good)}.note.bad{background:var(--bad)}
.well{display:flex;flex-direction:column;gap:10px;padding:14px;border-radius:14px;background:var(--recess);box-shadow:inset 0 1px 3px #0002}
.formrow{display:flex;flex-wrap:wrap;align-items:center;gap:10px}
.formrow .btn{height:40px;padding:0 20px 0 16px;font-size:15px}
.count{flex:1;font-size:13px;font-weight:650;font-variant-numeric:tabular-nums}
.shake{animation:shake .4s}
.sent{margin-top:18px;display:flex;flex-direction:column;gap:8px}
.sent h2,.subhead{margin:0 4px 2px;font-size:13px;font-weight:700;color:var(--soft)}
.sent ul,.ups{list-style:none;margin:0;padding:0;display:flex;flex-direction:column;gap:8px}
.sent li,.ups li{display:grid;grid-template-columns:36px minmax(0,1fr) auto;align-items:center;gap:4px 12px;padding:12px 14px;border-radius:12px;background:var(--card);border:1px solid var(--line);box-shadow:0 1px 2px #0001;animation:rise .35s var(--ease)}
.sent li>div{min-width:0;display:grid;gap:3px;grid-column:2/4}
.sent li>.i,.ups li>.i{width:22px;height:22px;justify-self:center;color:var(--deep)}
.sent li.ok>.i,.ups li.ok>.i{color:var(--good)}.sent li.no>.i,.ups li.no>.i{color:var(--bad)}
.sent b,.ups b{font-size:14px;color:var(--text);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.sent span,.ups .s{font-size:12px;color:var(--soft);overflow-wrap:anywhere}
.ups .s.err{color:var(--bad)}.ups .s.ok{color:var(--good)}
.ups .track{grid-column:2/4}
.ups li>div:nth-child(2){min-width:0;display:grid;gap:3px}
.track{height:6px;border-radius:3px;background:#ddd6ac;box-shadow:inset 0 1px 1px #0002;overflow:hidden}
.fill{height:100%;width:0;border-radius:3px;background:linear-gradient(90deg,#35aee0,#1683b0);transition:width .25s linear}
.active .fill{background-image:linear-gradient(45deg,#fff4 25%,transparent 25%,transparent 50%,#fff4 50%,#fff4 75%,transparent 75%),linear-gradient(90deg,#35aee0,#1683b0);background-size:18px 18px,100% 100%;animation:stripes .8s linear infinite}
.ok .fill{background:linear-gradient(90deg,#7fd08f,#3fa65a)}
.none{padding:28px 12px;text-align:center;color:var(--soft);font-weight:600}
/* Files */
.body{flex:1;min-height:0;display:flex;gap:14px;padding:14px 18px}
.pane{position:relative;flex:1;min-width:0;overflow:auto;display:flex;flex-direction:column;background:var(--recess);border-radius:14px;box-shadow:inset 0 1px 3px #0002;padding:10px;user-select:none;-webkit-user-select:none}
.loadline{position:absolute;left:0;right:0;top:0;height:3px;overflow:hidden;opacity:0;transition:opacity .2s;z-index:2}
.loadline.on{opacity:1}
.loadline::after{content:"";position:absolute;top:0;bottom:0;width:40%;background:var(--blue);border-radius:2px;animation:slide 1s var(--ease) infinite}
@keyframes slide{from{left:-40%}to{left:100%}}
.items{position:relative;flex:1 0 auto;transition:opacity .2s}
.items.dim{opacity:.55}
.item{position:relative;cursor:default;-webkit-touch-callout:none;transition:background .12s}
.item.anim{animation:rise .3s var(--ease) both;animation-delay:calc(var(--i,0)*14ms)}
.item.fresh{animation:fresh 1.8s ease}
@keyframes fresh{0%,30%{background:#bfe8c8}100%{background:transparent}}
.item.gone{animation:gone .28s var(--ease) forwards;pointer-events:none}
@keyframes gone{to{opacity:0;transform:scale(.92)}}
.items.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(118px,1fr));gap:6px;align-content:start}
.grid .item{display:flex;flex-direction:column;align-items:center;gap:3px;padding:10px 6px 8px;border-radius:12px;border:2px solid transparent;text-align:center}
.grid .item:hover{background:#ffffff38}
.grid .item.on{background:var(--pick);border-color:var(--ring);box-shadow:0 1px 3px #0002}
.grid .main{display:flex;flex-direction:column;align-items:center;gap:5px;max-width:100%}
.grid .ic{--s:72px}
.grid .nm{max-width:100%;font-size:13px;font-weight:600;color:var(--text);line-height:1.25;overflow-wrap:anywhere;display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical;overflow:hidden}
.grid .on .nm{font-weight:700}
.grid .mt{font-size:12px;color:var(--soft);font-variant-numeric:tabular-nums}
.grid .kd,.grid .sz,.grid .dt,.grid .pick{display:none}
.grid.picking .pick{display:block;position:absolute;top:8px;left:8px;width:20px;height:20px}
.lhead,.list .item{display:grid;grid-template-columns:34px minmax(0,2.6fr) minmax(0,1.2fr) 96px 150px;align-items:center;padding:0 10px}
.lhead{position:sticky;top:-10px;z-index:2;background:var(--recess);min-height:32px;font-size:12px;font-weight:700;color:var(--soft)}
.lhead>*{justify-self:start}
.lhead input{justify-self:center}
.lhead button{border:0;background:none;padding:0;font:700 12px system-ui,sans-serif;color:var(--soft);display:flex;align-items:center;gap:4px;cursor:pointer}
.lhead button.on{color:var(--text)}
.lhead .i{width:12px;height:12px;stroke-width:2.6}
.lhead .c-sz{justify-self:end}
.lhead .c-dt{padding-left:18px}
.items.list{display:grid;gap:2px;align-content:start}
.list .item{min-height:38px;border-radius:9px}
.list .item:nth-child(odd):not(.on){background:#ffffff1f}
.list .item:hover:not(.on){background:#ffffff4d}
.list .item.on{background:var(--pick);box-shadow:inset 0 0 0 1.5px var(--ring)}
.list .pick{justify-self:center}
.list .main{display:flex;align-items:center;gap:10px;min-width:0}
.list .ic{--s:28px}
.list .nm{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:14px;font-weight:600;color:var(--text)}
.list .kd,.list .dt{font-size:13px;color:var(--soft);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.list .sz{font-size:13px;text-align:right;font-variant-numeric:tabular-nums}
.list .dt{padding-left:18px;font-variant-numeric:tabular-nums}
.list .mt{display:none}
.nm mark{background:#ffe066;color:inherit;border-radius:3px;padding:0 1px}
.marquee{position:absolute;z-index:3;border:1px solid var(--blue);background:#1c93c222;border-radius:3px;pointer-events:none}
.empty{grid-column:1/-1;display:grid;justify-items:center;gap:8px;padding:56px 12px;color:var(--soft);font-weight:650}
.empty .i{width:40px;height:40px;color:#b3a96f}
.skel{display:block;border-radius:12px;background:linear-gradient(90deg,#cfc792 25%,#e4dcaa 50%,#cfc792 75%);background-size:200% 100%;animation:shimmer 1.2s linear infinite}
.grid .skel{height:118px}.list .skel{height:34px}
.dropzone{position:absolute;inset:0;z-index:4;display:none;flex-direction:column;align-items:center;justify-content:center;gap:14px;border-radius:14px;background:#dff1facc;outline:3px dashed var(--blue);outline-offset:-3px;pointer-events:none;animation:fadein .15s}
.pane.dropping .dropzone{display:flex}
.dropzone span{width:84px;height:84px;border-radius:22px;display:grid;place-items:center;background:linear-gradient(#2fa3d4,#136f97);box-shadow:inset 0 1px 0 #fff5,0 6px 18px #1c93c255;color:#fff}
.dropzone .i{width:40px;height:40px}
.dropzone b{font-size:20px;font-weight:750;color:var(--deep)}
/* Icons: one box, sized by --s, with the shape inside */
.ic{display:grid;place-items:center;flex:none;width:1em;height:1em;font-size:var(--s,42px);position:relative}
.ic>svg{width:.92em;height:.92em}
.ic .app{width:.84em;height:.84em;border-radius:.2em;display:grid;place-items:center;border:max(2px,.04em) solid #fff;box-shadow:0 1px 4px #0003;background:linear-gradient(#45b4e4,#1478a3);overflow:hidden}
.ic .app.k-app{background:linear-gradient(#6fc77f,#2f8a45)}
.ic .app.k-rom{background:linear-gradient(#ffb547,#d97a0a)}
.ic .app svg{width:.46em;height:.46em;fill:none;stroke:#fff;stroke-width:2.2;stroke-linecap:round;stroke-linejoin:round}
.ic .app.real{background:#fff}
.ic .app img{width:100%;height:100%;display:block;animation:fadein .3s ease}
.ic .pic{width:.94em;height:.7em;border-radius:.08em;border:max(2px,.04em) solid #fff;box-shadow:0 1px 4px #0004;background:linear-gradient(#7cc6ea 0,#bfe5f4 55%,#78b85a 56%,#4e9440 100%)}
.ic .doc{width:.72em;height:.92em;border-radius:.05em .2em .05em .05em;background:linear-gradient(#fff,#f1efe6);border:1px solid #c9c4ad;box-shadow:0 1px 3px #0002;display:flex;align-items:flex-end;justify-content:center;padding-bottom:.1em}
.ic .doc b{padding:.03em .09em;border-radius:.05em;color:#fff;background:#7d7563;font:800 max(7px,.14em)/1.15 system-ui,sans-serif;letter-spacing:.02em}
.ic .k-zip b{background:#9a6420}.ic .k-txt b{background:#665e47}.ic .k-media b{background:#7a5cc0}.ic .k-rom b{background:#d97a0a}
.list .ic .doc b{font-size:0;width:11px;height:3px;padding:0}
/* Inspector and sheet */
.insp{width:310px;flex:none;overflow-y:auto;display:flex;flex-direction:column;gap:14px}
.panel{display:flex;flex-direction:column;gap:14px;padding:18px;border-radius:14px;background:var(--card);border:1px solid var(--line);box-shadow:0 1px 3px #0001}
.insp .panel:first-child{flex:1}
.pv{display:flex;flex-direction:column;align-items:center;gap:10px;padding:18px 10px;border-radius:12px;background:#e9e4c6;text-align:center}
.pv .ic{--s:104px}
.pv h2{margin:0;font-size:18px;font-weight:750;color:var(--text);overflow-wrap:anywhere;line-height:1.25}
.pv .sub{font-size:13px;color:var(--soft);overflow-wrap:anywhere}
.stack{position:relative;width:132px;height:108px}
.stack .ic{--s:84px;position:absolute;top:12px;left:24px}
.stack .ic:nth-child(2){left:2px;transform:rotate(-8deg)}
.stack .ic:nth-child(3){left:46px;transform:rotate(8deg)}
.stack .ic:last-child{z-index:1}
.facts{display:grid;grid-template-columns:auto 1fr;gap:8px 12px;align-items:center;margin:0;font-size:14px}
.facts dt{justify-self:start;padding:2px 9px;border-radius:6px;background:var(--tag);color:#fff;font-size:12px;font-weight:650;white-space:nowrap}
.facts dd{margin:0;color:var(--text);overflow-wrap:anywhere;font-variant-numeric:tabular-nums}
.facts .mono{font-family:ui-monospace,Menlo,monospace;font-size:13px}
.facts .skel{grid-column:1/-1;height:14px;border-radius:6px}
.acts{display:grid;gap:8px;margin-top:auto}
.acts .btn{height:38px}
.acts .primary{height:40px;font-size:15px}
.acts .two{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}
.tmini{display:flex;flex-direction:column;gap:10px;padding:14px 16px 16px;border-radius:14px;background:var(--card);border:1px solid var(--line);box-shadow:0 6px 18px #0002;animation:rise .3s var(--ease)}
.tmini header{display:flex;align-items:center;gap:8px}
.tmini h2{margin:0;flex:1;font-size:15px;font-weight:750;color:var(--text)}
.tmini h2 a{color:inherit;text-decoration:none}
.tmini .row{display:grid;gap:5px;padding-top:8px;border-top:1px dotted #c9bf86}
.tmini .line{display:flex;gap:8px;min-width:0;font-size:12px;color:var(--soft);font-variant-numeric:tabular-nums}
.tmini .line b{flex:1;min-width:0;font-size:13px;color:var(--text);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tmini .line span:first-child{flex:1}
.pathbar{display:flex;flex-wrap:wrap;align-items:center;gap:8px 16px;padding:10px 18px 14px;border-top:1px dotted var(--dots);font-size:13px}
.crumbs{display:flex;flex-wrap:wrap;align-items:center;gap:2px;flex:1;min-width:0}
.crumbs a,.crumbs span{padding:2px 6px;border-radius:6px;font-weight:650;text-decoration:none;overflow-wrap:anywhere}
.crumbs span{color:var(--text)}
.crumbs .i{width:12px;height:12px;color:#9a9172;stroke-width:2.4}
.pathbar>span{color:var(--soft);font-variant-numeric:tabular-nums}
.gate{flex:1;display:grid;place-items:center;padding:24px 18px}
.gatecard{width:100%;max-width:440px;display:flex;flex-direction:column;align-items:center;gap:18px;padding:32px 28px 28px;border-radius:18px;background:var(--card);border:1px solid var(--line);box-shadow:0 10px 30px #0002;text-align:center;animation:rise .3s var(--ease)}
.gatecard .art{position:relative;width:96px;height:96px;display:grid;place-items:center}
.gatecard .art::before{content:"";position:absolute;inset:0;border-radius:50%;border:5px solid #e2dab0;border-top-color:var(--blue);animation:spin 1.4s linear infinite}
.gatecard.no .art::before{border-top-color:#e2dab0;animation:none}
.gatecard .art span{width:64px;height:64px;border-radius:16px;display:grid;place-items:center;background:linear-gradient(#35aee0,#1683b0);color:#fff;box-shadow:inset 0 1px 0 #fff5,0 2px 6px #0003}
.gatecard.no .art span{background:linear-gradient(#c9bf9a,#9a9172)}
.gatecard .art .i{width:32px;height:32px}
.gatecard h2{margin:0;font-size:22px;font-weight:750;color:var(--text)}
.gatecard p{margin:0;font-size:14px}
.gatecard .facts{align-self:stretch;padding:14px 16px;border-radius:12px;background:#e9e4c6;text-align:left}
#files.gated .tool{display:none}
#files.gated h1{color:#9a9172}
/* Bars that float */
.dock{position:fixed;left:0;right:0;bottom:0;z-index:15;display:grid;gap:8px;padding:0 12px calc(12px + env(safe-area-inset-bottom));pointer-events:none}
.dock>*{pointer-events:auto;max-width:46rem;width:100%;margin:0 auto;animation:rise .35s var(--spring)}
.selbar{display:flex;flex-wrap:wrap;align-items:center;gap:8px;padding:8px 8px 8px 14px;border-radius:14px;background:#fffdf0f2;backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px);border:1px solid #c9bf86;box-shadow:0 8px 28px #3a320f30}
.selbar b{margin-right:auto;color:var(--text);font-size:14px;font-variant-numeric:tabular-nums}
.selbar .acts2{display:flex;flex-wrap:wrap;gap:8px}
.selbar .btn.icon{width:40px;height:40px}
.selbar .btn.plain{border-color:transparent;background:none;box-shadow:none}
.menu{position:fixed;z-index:70;min-width:210px;max-width:calc(100vw - 16px);padding:5px;border-radius:12px;background:var(--cream);border:1px solid #c9bf86;box-shadow:0 12px 32px #0003;animation:fadein .12s}
.menu button{display:flex;align-items:center;gap:10px;width:100%;padding:7px 10px;border:0;border-radius:8px;background:none;font-size:14px;color:var(--text);text-align:left;cursor:pointer}
.menu button .i{width:16px;height:16px;color:var(--soft)}
.menu button:hover,.menu button:focus{outline:none;background:var(--blue);color:#fff}
.menu button:hover .i,.menu button:focus .i{color:#fff}
.menu .red{color:var(--red)}
.menu hr{border:0;border-top:1px dotted var(--dots);margin:4px 6px}
.dragcard{position:fixed;top:-200px;left:0;display:flex;align-items:center;gap:8px;padding:8px 12px;border-radius:10px;background:var(--cream);border:1px solid #c9bf86;box-shadow:0 6px 16px #0003;font-size:13px;font-weight:700;color:var(--text)}
.dragcard .ic{--s:28px}
.toasts{position:fixed;left:16px;right:16px;top:14px;z-index:80;display:flex;flex-direction:column;align-items:center;gap:8px;pointer-events:none}
.toast{display:flex;gap:9px;align-items:center;max-width:30rem;background:#1e1c16f0;color:#fff;padding:10px 16px;border-radius:12px;font-size:15px;box-shadow:0 8px 24px #0003;animation:toastin .35s var(--spring);transition:opacity .3s,transform .3s;overflow-wrap:anywhere}
.toast.good .i{color:#7fe08f}.toast.bad .i{color:#ff8a73}
.toast.out{opacity:0;transform:translateY(-10px) scale(.97)}
@keyframes toastin{from{opacity:0;transform:translateY(-14px) scale(.96)}}
/* Dialogs */
dialog{border:1px solid #b9ae73;border-radius:16px;padding:0;width:min(34rem,calc(100vw - 24px));max-height:calc(100vh - 24px);background:var(--card);color:var(--text);box-shadow:0 24px 60px #0005}
dialog[open]{animation:pop .28s var(--spring)}
dialog::backdrop{background:#2b261866;animation:fadein .2s}
.dbody{padding:20px 20px 4px}
dialog h2{margin:0 0 10px;font-size:18px;font-weight:750;overflow-wrap:anywhere}
dialog p{margin:0 0 12px;white-space:pre-line;overflow-wrap:anywhere;color:var(--ink)}
.warnbox{display:flex;gap:8px;align-items:flex-start;background:#fff0d6;border:1px solid #f0c27a;color:#8a4b00;border-radius:10px;padding:9px 11px;margin:0 0 12px;font-size:14px}
.err{color:var(--bad);font-size:14px;margin:8px 0 0}
.row2{display:flex;justify-content:flex-end;flex-wrap:wrap;gap:8px;margin:8px 20px 0;padding:12px 0 18px;border-top:1px dotted var(--dots)}
.row2 .left{margin-right:auto}
.clash{display:flex;flex-direction:column;gap:8px;margin-bottom:12px}
.clash>div{display:grid;grid-template-columns:48px minmax(0,1fr);align-items:center;gap:4px 14px;padding:12px 14px;border-radius:12px;background:#e9e4c6}
.clash .ic{--s:46px;grid-row:1/4}
.clash b{font-size:15px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.clash div div{display:flex;flex-wrap:wrap;align-items:center;gap:8px;font-size:13px;font-variant-numeric:tabular-nums}
.clash i{min-width:62px;text-align:center;padding:1px 8px;border-radius:6px;background:var(--tag);color:#fff;font:normal 650 12px system-ui,sans-serif}
.newer{padding:1px 8px;border-radius:999px;background:#e3f3e6;color:#24723b;font-size:12px;font-weight:700}
.clash .more{font-size:13px;color:var(--soft);padding-left:4px}
.picker{width:min(56rem,calc(100vw - 24px))}
.cols{display:flex;gap:6px;overflow-x:auto;height:min(340px,50vh);padding:6px;border-radius:12px;background:var(--recess);box-shadow:inset 0 1px 3px #0002;scroll-snap-type:x proximity}
.col{flex:0 0 220px;display:flex;flex-direction:column;gap:2px;padding:6px;border-radius:9px;background:var(--cream);overflow-y:auto;scroll-snap-align:end}
.col .spin{margin:12px auto}
.col .none,.col .err{padding:12px 6px;font-size:13px}
.fold{display:flex;align-items:center;gap:8px;width:100%;padding:6px 8px;border:0;border-radius:7px;background:none;font-size:14px;color:var(--text);text-align:left;cursor:pointer}
.fold svg:first-child{width:20px;height:20px;flex:none}
.fold span{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.fold .i{width:12px;height:12px;color:#9a9172}
.fold:hover{background:#0000000a}
.fold.on{background:var(--blue);color:#fff}
.fold.on .i{color:#fff}
.fold:disabled{opacity:.4;cursor:default}
.picker .crumbs{margin:12px 0 0}
.sheet .panel{border:0;box-shadow:none;background:none;padding:20px}
.close{position:absolute;right:12px;top:12px;width:34px;height:34px;border-radius:50%;border:0;background:#0000000d;color:var(--ink);display:grid;place-items:center;cursor:pointer;z-index:2}
.close:hover{background:#0000001a}
.viewer{width:min(64rem,calc(100vw - 16px));background:#141414;color:#fff;border-color:#000}
.viewer .stage{position:relative;display:grid;place-items:center;min-height:50vh;padding:12px}
.viewer img{max-width:100%;max-height:72vh;border-radius:6px;transition:opacity .2s}
.viewer img.loading{opacity:0}
.viewer .stage .spin{position:absolute;border-color:#444;border-top-color:#fff}
.viewer .vbar{display:flex;align-items:center;gap:8px;padding:10px 14px;border-top:1px solid #333;font-size:14px}
.viewer .vbar span{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.viewer .nav{position:absolute;top:50%;transform:translateY(-50%);width:44px;height:44px;border-radius:50%;border:0;background:#fff2;color:#fff;display:grid;place-items:center;cursor:pointer}
.viewer .nav:hover{background:#fff4}
.viewer .prev{left:10px}.viewer .next{right:10px}
.viewer .close{background:#fff2;color:#fff}
.editor{width:min(56rem,calc(100vw - 16px))}
.editor h2{display:flex;gap:8px;align-items:center;padding-right:40px}
.editor .dirty{width:9px;height:9px;border-radius:50%;background:var(--warn);opacity:0;transition:opacity .2s;flex:none}
.editor.changed .dirty{opacity:1}
.editor textarea{min-height:55vh;font-size:13px;tab-size:4}
.editor .row2{justify-content:space-between;align-items:center}
.muted{color:var(--soft);font-size:14px}
/* Without room for the details, the selection bar carries the actions */
@media (max-width:1079px){.insp{display:none}#upbtn{width:38px;padding:0}#upbtn .word{display:none}.search{flex:1 1 120px;min-width:0}}
@media (min-width:760px){.dock{left:248px}}
@media (min-width:1080px){.selbar{display:none}}
/* Phones: the places slide in, and the page scrolls as a whole */
@media (max-width:759px){
.app{display:block;height:auto}
.content,.view{overflow:visible}
.pad,.pane,.body{overflow:visible}
.side{position:fixed;left:0;top:0;bottom:0;z-index:50;transform:translateX(-105%);transition:transform .3s var(--ease);box-shadow:8px 0 30px #0003}
body.drawer .side{transform:none}
body.drawer .scrim{display:block;position:fixed;inset:0;z-index:45;background:#2b261855;animation:fadein .2s}
.mhead{display:flex;align-items:center;gap:8px;padding:10px 12px 0}
.mhead .logo b{font-size:17px}
.mhead .pill{margin-left:auto;max-width:58%}
.bar{min-height:0;padding:10px 12px;gap:8px}
.bar h1{flex:1;font-size:19px}
.bar .grow{display:none}
.brk{display:block;flex-basis:100%;height:0;order:5}
.nav2{order:1}.bar h1{order:2}#mkdir{order:3}#upbtn{order:4;width:38px;padding:0}#upbtn .word{display:none}
.search{order:6;flex:1 1 120px;min-width:0}.views{order:7}#sortbtn{order:8;padding:0 10px}
.body{padding:10px 8px}
.pane{padding:6px}
.items.grid{grid-template-columns:repeat(auto-fill,minmax(98px,1fr));gap:4px}
.grid .ic{--s:60px}
.lhead,.list .item{grid-template-columns:30px minmax(0,1fr) 84px;padding:0 6px}
.list .kd,.list .dt,.lhead .c-kd,.lhead .c-dt{display:none}
.pathbar{padding:10px 14px 120px}
.pad{padding:14px 12px 110px}
dialog.sheet{margin:auto 0 0;width:100vw;max-width:100vw;border-radius:20px 20px 0 0;padding-bottom:env(safe-area-inset-bottom)}
.cols{height:min(300px,46vh)}
.col{flex-basis:72%}
.selbar .acts2{order:2;flex-basis:100%}
.selbar .acts2 .primary{flex:1}
}
@media (prefers-reduced-motion:reduce){*,*::before,*::after{animation:none!important;transition:none!important}}
</style></head><body>
<div class="app">
<aside class="side" id="side">
<div class="brand"><a class="logo" href="https://ankush.one/3dsDL/" target="_blank" rel="noopener"><img class="logoimg" alt=""><b>3dsDL</b></a>
<nav class="ext" id="ext"><a href="https://ankush.one/3dsDL/" target="_blank" rel="noopener">Website</a><a href="https://github.com/ankushKun/3dsDL" target="_blank" rel="noopener">GitHub</a></nav>
<div class="pill" data-state="connecting" role="status" aria-live="polite"><i class="dot"></i><span>Connecting...</span></div></div>
<nav class="nav" aria-label="Receive"><a href="#links" data-view="links"><svg class="i" viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="8"/><path d="M4 12h16M12 4c2.4 2.3 3.5 5 3.5 8s-1.1 5.7-3.5 8c-2.4-2.3-3.5-5-3.5-8s1.1-5.7 3.5-8z"/></svg><span>Send Links</span></a><a href="#transfers" data-view="transfers"><svg class="i" viewBox="0 0 24 24" aria-hidden="true"><path d="M12 4v10M8 10l4 4 4-4M4 15v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3"/></svg><span>Transfers</span><b class="badge" id="tbadge" hidden></b></a></nav>
<section class="group places"><h3>SD Card</h3><nav class="nav" id="places"><a href="#files:%2F" data-place="/" data-drop="/"><svg class="i" viewBox="0 0 24 24" aria-hidden="true"><path d="M7 3h7l5 5v11a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2zM9 3.5v3M12 3.5v3"/></svg><span>SD Card</span></a></nav></section>
<section class="group places" id="favgroup" hidden><h3>Favourites</h3><nav class="nav" id="favs"></nav></section>
<div class="storage" id="storage" hidden><div><b>SD card</b><span id="free"></span></div><div class="meter" id="meter"><i></i></div><span id="used"></span></div>
</aside>
<div class="scrim" id="scrim"></div>
<div class="content">
<header class="mhead"><button class="btn icon" id="menubtn" aria-label="Places"></button><a class="logo" href="#links"><img class="logoimg" alt=""><b>3dsDL</b></a><div class="pill" data-state="connecting" role="status"><i class="dot"></i><span>Connecting...</span></div></header>
<div class="banner" id="offline" hidden><span class="spin"></span><span>Can't reach the 3DS. Keep 3dsDL open on Receive; this page reconnects by itself.</span></div>
<section class="view" id="links">
<header class="bar"><h1>Send Links</h1></header>
<div class="pad">
)PAGE";

const char *const PageEnd = R"PAGE(<form method="post" action="/send" class="well" id="sendform">
<textarea name="links" id="linkbox" aria-label="Links" placeholder="https://..." autofocus required></textarea>
<div class="formrow"><span class="count" id="linkcount"></span><button class="btn primary" type="submit" id="sendbtn">Send</button></div></form>
<section class="sent" id="sentbox" hidden><h2>Sent</h2><ul id="sent"></ul></section>
</div>
</section>
<section class="view" id="transfers" hidden>
<header class="bar"><h1>Transfers</h1><span class="grow"></span><button class="btn small" id="upcancel" hidden>Stop All</button><button class="btn small" id="upclear" hidden>Clear Finished</button></header>
<div class="pad"><ul class="ups" id="uplist"></ul><p class="none" id="upnone">No transfers</p>
<label class="check" style="margin-top:16px"><input type="checkbox" id="offer" checked>Offer to install CIAs on the 3DS once they arrive</label></div>
</section>
<section class="view" id="files" hidden>
<header class="bar"><div class="seg tool nav2"><button type="button" id="back" aria-label="Back"></button><button type="button" id="fwd" aria-label="Forward"></button></div>
<h1 id="title">SD Card</h1><span class="grow"></span>
<div class="seg tool views" role="group" aria-label="View"><button type="button" id="asgrid" aria-label="Icons"></button><button type="button" id="aslist" aria-label="List"></button></div>
<button class="btn tool" id="sortbtn" aria-haspopup="menu"></button>
<label class="search tool"><input type="search" id="filter" autocomplete="off" aria-label="Search this folder"></label>
<button class="btn icon tool" id="mkdir" aria-label="New Folder" title="New Folder"></button>
<label class="btn primary tool" id="upbtn" title="Upload files (U)"><input type="file" id="pick" multiple hidden></label><span class="brk"></span><input type="file" id="pickdir" webkitdirectory multiple hidden></header>
<div class="ccard" id="ccard" hidden></div>
<div class="gate" id="gate" hidden></div>
<div class="body" id="browser" hidden>
<section class="pane" id="pane" aria-label="Files"><div class="loadline" id="loadline"></div>
<div class="lhead" id="lhead" hidden><input type="checkbox" id="all" aria-label="Select all"><button type="button" data-sort="name">Name</button><button type="button" class="c-kd" data-sort="kind">Kind</button><button type="button" class="c-sz" data-sort="size">Size</button><button type="button" class="c-dt" data-sort="time">Changed</button></div>
<div class="items grid" id="items" role="listbox" aria-multiselectable="true" aria-label="Files"></div>
<div class="dropzone" id="dropzone"><span></span><b></b></div></section>
<aside class="insp" id="insp"><div class="panel" id="detail"></div><div class="tmini" hidden></div></aside>
</div>
<footer class="pathbar" id="pathbar" hidden><nav class="crumbs" id="crumbs" aria-label="Path"></nav><span id="count"></span></footer>
</section>
</div>
</div>
<div class="dock"><div class="tmini" hidden></div><div class="selbar" id="selbar" hidden></div></div>
<div class="toasts" id="toasts"></div>
<div class="dragcard" id="dragcard"></div>
<dialog id="dlg"><div class="dbody"><h2 id="dt"></h2><div class="warnbox" id="dw" hidden><span class="dwi"></span><span></span></div><p id="dp"></p><div id="dx"></div>
<input type="text" id="di" autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false"><p class="err" id="de" hidden></p></div><div class="row2" id="db"></div></dialog>
<dialog id="picker" class="picker"><div class="dbody"><h2 id="ptitle"></h2><div class="cols" id="pcols"></div><nav class="crumbs" id="pcrumbs"></nav></div>
<div class="row2"><button type="button" class="btn left" id="pmkdir"></button><button type="button" class="btn" id="pcancel">Cancel</button><button type="button" class="btn" id="pcopy">Copy</button><button type="button" class="btn" id="pmove">Move</button></div></dialog>
<dialog id="sheet" class="sheet"></dialog>
<dialog id="viewer" class="viewer"><button class="close" id="vclose" aria-label="Close"></button><div class="stage"><span class="spin" id="vspin"></span><img id="vimg" alt=""><button class="nav prev" id="vprev" aria-label="Previous"></button><button class="nav next" id="vnext" aria-label="Next"></button></div>
<div class="vbar"><span id="vname"></span><a class="btn small" id="vdl" download>Download</a></div></dialog>
<dialog id="editor" class="editor"><button class="close" id="eclose" aria-label="Close"></button><div class="dbody"><h2><span id="ename"></span><i class="dirty" title="Not saved"></i></h2>
<textarea id="etext" spellcheck="false" autocorrect="off" autocapitalize="off"></textarea><div id="ecode" hidden></div><p class="err" id="eerr" hidden></p></div>
<div class="row2"><span class="muted" id="einfo"></span><span><button class="btn" id="ecancel">Close</button> <button class="btn primary" id="esave">Save</button></span></div></dialog>
<script>
'use strict';
const $ = s => document.querySelector(s);
const $$ = s => [...document.querySelectorAll(s)];
const enc = encodeURIComponent;
const H = {'X-Requested-With': '3dsDL'};
const esc = t => String(t).replace(/[&<>"]/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));
const store = {
  get(key, fallback) { try { const v = localStorage.getItem('3dsdl.' + key); return v === null ? fallback : JSON.parse(v); } catch (e) { return fallback; } },
  set(key, value) { try { localStorage.setItem('3dsdl.' + key, JSON.stringify(value)); } catch (e) {} }
};
// The details column shows from this width; below it the selection bar
// carries the actions. Touch screens get taps and long presses, mice get
// clicks, double clicks, drags and a box to select with.
const wide = matchMedia('(min-width: 1080px)'), fine = matchMedia('(pointer: fine)');
const iOS = /iPhone|iPad|iPod/.test(navigator.userAgent), mac = /Mac|iPhone|iPad/.test(navigator.platform || '');

// --- Icons ---------------------------------------------------------------------
const ICONS = {
  upload: '<path d="M12 15V5M8 9l4-4 4 4M4 15v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3"/>',
  folder: '<path d="M3.5 7.5a1.5 1.5 0 0 1 1.5-1.5h4l2 2h8a1.5 1.5 0 0 1 1.5 1.5v8a1.5 1.5 0 0 1-1.5 1.5H5a1.5 1.5 0 0 1-1.5-1.5z"/>',
  folderplus: '<path d="M3.5 7.5a1.5 1.5 0 0 1 1.5-1.5h4l2 2h8a1.5 1.5 0 0 1 1.5 1.5v8a1.5 1.5 0 0 1-1.5 1.5H5a1.5 1.5 0 0 1-1.5-1.5zM12 11v5M9.5 13.5h5"/>',
  search: '<circle cx="11" cy="11" r="6.5"/><path d="M16 16l4 4"/>',
  download: '<path d="M12 4v12M7 11l5 5 5-5M4 20h16"/>',
  eye: '<path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>',
  edit: '<path d="M4 20h4L19 9l-4-4L4 16zM14 6l4 4"/>',
  install: '<path d="M12 3v10M8 9l4 4 4-4M4 15v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3"/>',
  copy: '<rect x="9" y="9" width="11" height="11" rx="2"/><path d="M5 15V6a2 2 0 0 1 2-2h8"/>',
  rename: '<path d="M4 7V5h16v2M12 5v14M9 19h6"/>',
  move: '<path d="M5 12h14M13 6l6 6-6 6"/>',
  paste: '<rect x="5" y="5" width="14" height="16" rx="2"/><path d="M9 5V3.5h6V5M9 11h6M9 15h4"/>',
  trash: '<path d="M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13"/>',
  x: '<path d="M6 6l12 12M18 6 6 18"/>',
  check: '<path d="M5 12l5 5L20 7"/>',
  warn: '<path d="M12 3 2 21h20zM12 10v4M12 17.5v.5"/>',
  info: '<circle cx="12" cy="12" r="9"/><path d="M12 11v6M12 7.5v.5"/>',
  link: '<path d="M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1"/>',
  lock: '<rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/>',
  left: '<path d="M15 5l-7 7 7 7"/>',
  right: '<path d="M9 5l7 7-7 7"/>',
  console: '<rect x="6" y="2.5" width="12" height="19" rx="2"/><path d="M8.5 5h7v5h-7zM10 15h4M12 13v4"/>',
  retry: '<path d="M4 4v6h6M4 10a8 8 0 1 1 2.3 5.7"/>',
  down: '<path d="M6 9l6 6 6-6"/>',
  upward: '<path d="M6 15l6-6 6 6"/>',
  out: '<path d="M7 17 17 7M9 7h8v8"/>',
  grid: '<rect x="4" y="4" width="6.5" height="6.5" rx="1.5"/><rect x="13.5" y="4" width="6.5" height="6.5" rx="1.5"/><rect x="4" y="13.5" width="6.5" height="6.5" rx="1.5"/><rect x="13.5" y="13.5" width="6.5" height="6.5" rx="1.5"/>',
  list: '<path d="M9 6h11M9 12h11M9 18h11M4.5 6h.5M4.5 12h.5M4.5 18h.5"/>',
  panel: '<rect x="3.5" y="4.5" width="17" height="15" rx="2"/><path d="M9 4.5v15"/>',
  link2: '<path d="M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1"/>',
  star: '<path d="M12 3.6l2.29 5.84 6.27.38-4.85 3.99 1.58 6.07L12 16.5l-5.29 3.38 1.58-6.07-4.85-3.99 6.27-.38z"/>'
};
const svg = (name, cls = '') => '<svg class="i' + (cls ? ' ' + cls : '') + '" viewBox="0 0 24 24" aria-hidden="true">' + ICONS[name] + '</svg>';
const FOLDER = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M2.5 7a1.8 1.8 0 0 1 1.8-1.8h4.6l2 2h8.8a1.8 1.8 0 0 1 1.8 1.8v9.2a1.8 1.8 0 0 1-1.8 1.8H4.3a1.8 1.8 0 0 1-1.8-1.8z" fill="#f4b04a" stroke="#d07f0b" stroke-width="1"/><path d="M2.5 10h19" stroke="#d07f0b" stroke-width="1" opacity=".5"/></svg>';
const CUBE = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 8l8-4 8 4-8 4zM4 8v8l8 4 8-4V8M12 12v8"/></svg>';
for (const img of $$('.logoimg')) img.src = document.querySelector('link[rel=icon]').href;
for (const a of $$('#ext a')) a.insertAdjacentHTML('beforeend', svg('out'));
for (const [id, name] of [['back', 'left'], ['fwd', 'right'], ['asgrid', 'grid'], ['aslist', 'list'], ['mkdir', 'folderplus'], ['menubtn', 'panel'], ['vclose', 'x'], ['eclose', 'x'], ['vprev', 'left'], ['vnext', 'right']]) $('#' + id).innerHTML = svg(name);
$('#upbtn').insertAdjacentHTML('afterbegin', svg('upload') + '<span class="word">Upload</span>');
$('#pmkdir').innerHTML = svg('folderplus') + 'New Folder';
$('.search').insertAdjacentHTML('afterbegin', svg('search'));
$('.dwi').outerHTML = svg('warn');
$('#dropzone span').innerHTML = svg('upload');
$('#sendbtn').insertAdjacentHTML('afterbegin', svg('move'));

// --- Paths and files -----------------------------------------------------------
const join = (dir, name) => dir === '/' ? '/' + name : dir + '/' + name;
const parent = p => p.slice(0, p.lastIndexOf('/')) || '/';
const base = p => p.slice(p.lastIndexOf('/') + 1);
const ext = n => { const i = n.lastIndexOf('.'); return i > 0 ? n.slice(i + 1).toLowerCase() : ''; };
const folderName = p => p === '/' ? 'SD Card' : base(p);
// True when path is folder itself or anything inside it.
const inside = (path, folder) => path === folder || path.startsWith(folder === '/' ? '/' : folder + '/');
const plural = (n, one) => n + ' ' + one + (n === 1 ? '' : 's');
const what = paths => paths.length === 1 ? base(paths[0]) : paths.length + ' items';
const byName = (a, b) => a.name.localeCompare(b.name, undefined, {numeric: true, sensitivity: 'base'});
function size(b) {
  if (b < 0) return '';
  const units = ['B', 'KiB', 'MiB', 'GiB'];
  let i = 0;
  while (b >= 1024 && i < 3) { b /= 1024; i++; }
  return (i ? b.toFixed(b < 10 ? 2 : 1) : b) + ' ' + units[i];
}
// The console's clock has no time zone, so its times are shown as stored.
function when(t) {
  if (!t) return '';
  const d = new Date(t * 1e3), z = n => String(n).padStart(2, '0');
  return d.getUTCFullYear() + '-' + z(d.getUTCMonth() + 1) + '-' + z(d.getUTCDate()) + ' ' + z(d.getUTCHours()) + ':' + z(d.getUTCMinutes());
}
// As the console says it (units::timeLeft): counting down under ten
// seconds, then in fives, then in minutes.
function duration(s) {
  if (!isFinite(s) || s < 0) return '';
  if (s < 10) return Math.max(1, Math.round(s)) + ' s';
  if (Math.ceil(s / 5) * 5 < 60) return Math.ceil(s / 5) * 5 + ' s';
  const m = Math.max(1, Math.round(s / 60));
  return m < 60 ? m + ' min' : Math.floor(m / 60) + ' h' + (m % 60 ? ' ' + m % 60 + ' min' : '');
}
const FAMILY = {cia: 'cia', zcia: 'cia', '3dsx': 'app', zip: 'zip', '7z': 'zip', rar: 'zip', tar: 'zip', gz: 'zip', tgz: 'zip', xz: 'zip', bz2: 'zip', zst: 'zip',
  txt: 'txt', ini: 'txt', cfg: 'txt', json: 'txt', xml: 'txt', log: 'txt', md: 'txt', yml: 'txt', yaml: 'txt', lua: 'txt', nds: 'rom', '3ds': 'rom', cci: 'rom',
  firm: 'rom', tik: 'rom', smdh: 'rom', mp3: 'media', ogg: 'media', wav: 'media', flac: 'media', m4a: 'media', bcstm: 'media', mp4: 'media', pdf: 'media'};
const KINDS = {cia: 'CIA', zcia: 'CIA', '3dsx': 'Homebrew', nds: 'DS game', '3ds': '3DS game', cci: '3DS game', firm: 'Firmware', tik: 'Ticket', smdh: 'Icon',
  mp4: 'Video', pdf: 'PDF', zip: 'Archive', '7z': 'Archive', rar: 'Archive', tar: 'Archive', gz: 'Archive', tgz: 'Archive', xz: 'Archive', bz2: 'Archive', zst: 'Archive'};
const TEXT = new Set(['txt', 'ini', 'cfg', 'json', 'xml', 'log', 'md', 'yml', 'yaml', 'lua', 'csv', 'conf', 'sh', 'py', 'js', 'html', 'htm', 'css', 'opt']);
const PICTURES = new Set(['png', 'jpg', 'jpeg', 'gif', 'bmp', 'webp']);
const MEDIA = new Set(['mp3', 'ogg', 'wav', 'mp4', 'pdf']);
const TITLED = new Set(['cia', 'zcia', '3dsx', 'smdh', '3ds', 'cci', 'nds']);
const CIAS = new Set(['cia', 'zcia']);
function kindOf(e) {
  if (e.dir) return 'Folder';
  const x = ext(e.name);
  return KINDS[x] || (PICTURES.has(x) ? 'Picture' : TEXT.has(x) ? 'Text' : FAMILY[x] === 'media' ? 'Audio' : x ? x.toUpperCase() + ' file' : 'File');
}
// The picture for an entry, scaled by the --s of wherever it goes.
function glyph(e) {
  if (e.dir) return '<span class="ic">' + FOLDER + '</span>';
  const x = ext(e.name), family = FAMILY[x] || 'file';
  if (TITLED.has(x)) return '<span class="ic"><span class="app k-' + family + '">' + CUBE + '</span></span>';
  if (PICTURES.has(x)) return '<span class="ic"><span class="pic"></span></span>';
  return '<span class="ic"><span class="doc k-' + family + '"><b>' + esc((x || 'file').slice(0, 4).toUpperCase()) + '</b></span></span>';
}
const fileUrl = (path, download) => '/api/file?' + (download ? 'download=1&' : '') + 'path=' + enc(path);
// Places where a slip can stop the console from starting or lose games.
function risk(paths) {
  for (const p of paths) {
    const l = p.toLowerCase();
    if (l === '/boot.firm' || inside(l, '/luma') || inside(l, '/boot9strap'))
      return 'This is part of what starts custom firmware. Changing it can stop the 3DS from starting.';
    if (inside(l, '/nintendo 3ds'))
      return 'This folder holds installed software, saves and extra data. Changing it can break games.';
    if (l === '/boot.3dsx' || inside(l, '/3ds/3dsdl'))
      return 'The Homebrew Launcher or 3dsDL itself uses this.';
  }
  return '';
}

// --- The lane: one request at a time, most urgent first ------------------------
// 0: what the person just asked for, 1: uploads and lists for them,
// 2: status checks, 3: icons.
const lane = {busy: false, queue: []};
function run(task, priority = 0, tag = '') {
  return new Promise((resolve, reject) => {
    let at = lane.queue.findIndex(job => job.priority > priority);
    if (at < 0) at = lane.queue.length;
    lane.queue.splice(at, 0, {task, priority, tag, resolve, reject});
    pump();
  });
}
async function pump() {
  if (lane.busy || !lane.queue.length) return;
  const job = lane.queue.shift();
  lane.busy = true;
  try { job.resolve(await job.task()); } catch (e) { job.reject(e); } finally { lane.busy = false; pump(); }
}
function quiet() { const e = new Error(''); e.quiet = true; return e; }
const sleep = ms => new Promise(r => setTimeout(r, ms));
// A request that cannot reach the 3DS is tried again a few times before the
// page calls it lost: a tab in the background has its connections held up
// and its timers slowed, and one dropped request is not a lost console.
// Only requests that do the same the second time are repeated: reads, and
// those passed retry.
async function call(method, url, body, options = {}) {
  const request = {method, headers: {...H}};
  if (typeof body === 'string') request.body = body;
  else if (body) { request.headers['Content-Type'] = 'application/x-www-form-urlencoded'; request.body = new URLSearchParams(body).toString(); }
  const tries = method === 'GET' || options.retry ? 4 : 1;
  let response;
  for (let n = 1; !response; n++) {
    const stop = new AbortController();
    const timer = setTimeout(() => stop.abort(), options.timeout || 20000);
    request.signal = stop.signal;
    try { response = await fetch(url, request); } catch (e) {
      if (n >= tries) { offline(true); throw new Error('Lost the connection to the 3DS.'); }
      await sleep(750 * n);
    } finally { clearTimeout(timer); }
  }
  offline(false);
  if (options.raw) return response;
  let json = {};
  try { json = await response.json(); } catch (e) {}
  if (response.status === 403 && json.access) { gate(json.access); throw quiet(); }
  if (!response.ok) throw new Error(json.error || 'The 3DS answered ' + response.status + '.');
  return json;
}
const api = (method, url, body, priority = 0, options) => run(() => call(method, url, body, options), priority);

// --- Toasts, menus and dialogs -------------------------------------------------
function toast(text, kind = '') {
  const e = document.createElement('div');
  e.className = 'toast ' + kind;
  e.innerHTML = svg(kind === 'bad' ? 'warn' : kind === 'good' ? 'check' : 'info');
  e.append(text);
  $('#toasts').append(e);
  setTimeout(() => { e.classList.add('out'); setTimeout(() => e.remove(), 320); }, kind === 'bad' ? 5500 : 3800);
}
function fail(e) { if (e && e.message) toast(e.message, 'bad'); }
// A menu at a point or under an element. Items are {label, icon, fn, red,
// checked} or '-' for a line.
let menuOpen = null;
function popup(at, items) {
  closeMenu();
  const m = document.createElement('div');
  m.className = 'menu';
  m.setAttribute('role', 'menu');
  for (const it of items) {
    if (it === '-') { m.append(document.createElement('hr')); continue; }
    const b = document.createElement('button');
    b.type = 'button';
    b.setAttribute('role', 'menuitem');
    if (it.red) b.className = 'red';
    b.innerHTML = it.checked !== undefined ? (it.checked ? svg('check') : '<span class="i"></span>') : svg(it.icon);
    b.append(it.label);
    b.onclick = () => { closeMenu(); it.fn(); };
    m.append(b);
  }
  document.body.append(m);
  const r = at instanceof Element ? at.getBoundingClientRect() : null;
  const x = r ? r.right - m.offsetWidth : at.x;
  let y = r ? r.bottom + 6 : at.y;
  if (y + m.offsetHeight > innerHeight - 8) y = (r ? r.top - 6 : at.y) - m.offsetHeight;
  m.style.left = Math.max(8, Math.min(x, innerWidth - m.offsetWidth - 8)) + 'px';
  m.style.top = Math.max(8, y) + 'px';
  menuOpen = m;
  m.querySelector('button').focus();
}
function closeMenu() { if (menuOpen) { menuOpen.remove(); menuOpen = null; } }
addEventListener('pointerdown', e => { if (menuOpen && !menuOpen.contains(e.target)) closeMenu(); }, true);
addEventListener('resize', closeMenu);
// Asks a question. With o.action the main button runs it, shows a spinner
// and keeps the dialog open with the error when it fails. o.body is shown
// under the text; o.left puts the first button on the left.
function ask(title, text, o = {}) {
  return new Promise(resolve => {
    const d = $('#dlg'), input = $('#di'), err = $('#de'), row = $('#db'), w = $('#dw');
    $('#dt').textContent = title;
    $('#dp').textContent = text || '';
    $('#dp').hidden = !text;
    $('#dx').textContent = '';
    if (o.body) $('#dx').append(o.body);
    w.hidden = !o.warn;
    w.lastChild.textContent = o.warn || '';
    input.hidden = o.input === undefined;
    input.value = o.input || '';
    err.hidden = true;
    row.textContent = '';
    const labels = o.buttons || ['Cancel', 'OK'], main = labels.length - 1;
    let done = false;
    const finish = value => { if (done) return; done = true; d.close(); resolve(value); };
    const press = async n => {
      if (n !== main || !o.action) return finish(o.input !== undefined ? (n === main ? input.value : null) : n);
      const b = row.children[n];
      b.classList.add('busy');
      err.hidden = true;
      try { const r = await o.action(input.value); finish(r === undefined ? true : r); } catch (e) {
        b.classList.remove('busy');
        if (e.quiet) return finish(null);
        err.textContent = e.message;
        err.hidden = false;
        if (!input.hidden) input.focus();
      }
    };
    labels.forEach((label, n) => {
      const b = document.createElement('button');
      b.type = 'button';
      b.textContent = label;
      b.className = 'btn' + (n === main ? (o.danger ? ' danger' : ' primary') : '') + (n === 0 && o.left ? ' left' : '');
      b.onclick = () => press(n);
      row.append(b);
    });
    input.onkeydown = e => { if (e.key === 'Enter') { e.preventDefault(); press(main); } };
    d.oncancel = e => { e.preventDefault(); if (!row.querySelector('.busy')) finish(o.input !== undefined ? null : -1); };
    d.showModal();
    if (!input.hidden) {
      input.focus();
      const dot = input.value.lastIndexOf('.');
      input.setSelectionRange(o.select ? 0 : input.value.length, o.select && dot > 0 ? dot : input.value.length);
    } else row.children[o.danger ? 0 : main].focus();
  });
}

// --- What the console is doing -------------------------------------------------
const S = {state: 'connecting', text: '', access: 'none', you: '', seq: -1, expect: [], offline: false, timer: 0, files: true};
const SAYS = {ready: '3DS ready', asking: 'Asking on the 3DS', choosing: 'Waiting for you on the 3DS', installing: 'Installing on the 3DS',
  downloading: 'Downloading on the 3DS', busy: 'The 3DS is busy', offline: "Can't reach the 3DS", connecting: 'Connecting...'};
const HEADS = {choosing: 'Over to you on the 3DS', asking: 'The 3DS is asking a question', installing: 'Installing on the 3DS', downloading: 'Downloading on the 3DS', busy: 'The 3DS is busy'};
// What this page itself is doing on the console, such as copying, said in
// the status pill like an upload.
const W = {text: ''};
function showStatus() {
  const own = U.current ? U.pill : W.text;
  const state = S.offline ? 'offline' : own ? 'sending' : S.state;
  const text = S.offline ? SAYS.offline : own || S.text || SAYS[S.state] || S.state;
  for (const p of $$('.pill')) {
    p.dataset.state = state;
    p.lastChild.textContent = text;
    p.title = text;
  }
  const c = $('#ccard');
  const show = !S.offline && S.access === 'allowed' && !!HEADS[S.state];
  if (show) {
    const key = S.state + S.text;
    if (c.dataset.key !== key) {
      c.dataset.key = key;
      const art = S.state === 'installing' || S.state === 'downloading' ? '<span class="spin"></span>' : svg('console');
      c.innerHTML = '<div class="art">' + art + '</div><div><b>' + esc(HEADS[S.state]) + '</b><span>' + esc(S.text) + '</span></div>';
    }
  } else c.dataset.key = '';
  c.hidden = !show;
  for (const span of $$('#sent li.wait span')) span.textContent = S.state === 'ready' ? 'Sent to the 3DS.' : (S.text || SAYS[S.state]);
}
function onStatus(j) {
  const before = S.access;
  Object.assign(S, {you: j.you, access: j.access, state: j.state, text: j.text, files: j.files});
  if (S.seq >= 0 && j.resultSeq !== S.seq) {
    const text = j.result || (j.resultOk ? 'Done on the 3DS.' : 'Not done on the 3DS.');
    toast(text, j.resultOk ? 'good' : 'bad');
    const li = S.expect.shift();
    if (li) {
      li.className = j.resultOk ? 'ok' : 'no';
      li.querySelector('.i').outerHTML = svg(j.resultOk ? 'check' : 'warn');
      li.querySelector('span').textContent = text;
    }
    if (browsing()) load(F.cwd, true);
  }
  S.seq = j.resultSeq;
  const you = $('#gateyou');
  if (you && S.you) { you.textContent = S.you; you.parentNode.hidden = false; }
  if (onFiles() && before !== 'allowed' && j.access === 'allowed' && !$('#gate').hidden) load(F.cwd);
  if (j.access === 'denied' && onFiles() && !$('#gate').hidden) gate('denied');
  showStatus();
}
async function poll() {
  clearTimeout(S.timer);
  const queued = lane.queue.some(job => job.tag === 'status');
  if (!document.hidden && !U.current && !queued) {
    try { onStatus(await run(() => call('GET', '/api/status', undefined, {timeout: 6000}), 2, 'status')); } catch (e) {}
  }
  const quick = S.offline || S.state !== 'ready' || S.expect.length || S.access === 'pending';
  S.timer = setTimeout(poll, quick ? 1000 : 3000);
}
function offline(on) {
  if (S.offline === on) return;
  S.offline = on;
  $('#offline').hidden = !on;
  showStatus();
  if (on) { clearTimeout(S.timer); S.timer = setTimeout(poll, 1500); }
  else if (browsing()) { toast('Connected to the 3DS again.', 'good'); load(F.cwd, true); }
}
document.addEventListener('visibilitychange', () => { if (!document.hidden) poll(); });

// --- Views and places --------------------------------------------------------------
const onFiles = () => location.hash.startsWith('#files');
const browsing = () => onFiles() && !$('#browser').hidden;
const viewOf = () => onFiles() ? 'files' : location.hash === '#transfers' ? 'transfers' : 'links';
// Back and Forward go through the folders opened here, as in Finder.
const N = {stack: [], at: -1};
function visited(path) {
  if (N.stack[N.at] === path) return;
  if (N.stack[N.at - 1] === path) N.at--;
  else if (N.stack[N.at + 1] === path) N.at++;
  else { N.stack = N.stack.slice(0, N.at + 1); N.stack.push(path); N.at++; }
}
function route() {
  const view = viewOf();
  for (const v of ['links', 'transfers', 'files']) $('#' + v).hidden = v !== view;
  document.body.classList.remove('drawer');
  closeMenu();
  if (view === 'files') {
    let path = '/';
    try { path = decodeURIComponent(location.hash.slice(7)) || '/'; } catch (e) {}
    visited(path);
    if (!S.files) gate('off'); else load(path);
  }
  sideMarks();
  selbar();
  progress();
}
const go = path => { location.hash = '#files:' + enc(path); };
addEventListener('hashchange', route);
$('#back').onclick = () => { if (N.at > 0) go(N.stack[N.at - 1]); };
$('#fwd').onclick = () => { if (N.at < N.stack.length - 1) go(N.stack[N.at + 1]); };
$('#menubtn').onclick = () => document.body.classList.toggle('drawer');
$('#scrim').onclick = () => document.body.classList.remove('drawer');
// The sidebar's folders and Files' favourites, asked for once the 3DS lets
// this device see its files. A file opens its folder with it selected.
const PL = {asked: false};
async function places() {
  if (PL.asked) return;
  PL.asked = true;
  let j;
  try { j = await api('GET', '/api/places', undefined, 2); } catch (e) { PL.asked = false; return; }
  const row = (p, icon) => '<a href="#files:' + enc(p.dir ? p.path : parent(p.path)) + '" data-place="' + esc(p.path) + '"' + (p.dir ? ' data-drop="' + esc(p.path) + '"' : ' data-file="1"') +
    '>' + svg(icon, icon === 'star' ? 'solid star' : '') + '<span>' + esc(p.name) + '</span></a>';
  const root = $('#places').firstElementChild.outerHTML;
  $('#places').innerHTML = root + j.places.map(p => row(p, 'folder')).join('');
  $('#favs').innerHTML = j.favorites.map(p => row(p, 'star')).join('');
  $('#favgroup').hidden = !j.favorites.length;
  sideMarks();
}
$('#side').addEventListener('click', e => {
  if (e.target.closest('a')) document.body.classList.remove('drawer');
  const a = e.target.closest('[data-file]');
  if (!a) return;
  e.preventDefault();
  F.pick = a.dataset.place;
  if (onFiles() && parent(F.pick) === F.cwd) { selectOnly(F.pick); F.pick = ''; } else go(parent(F.pick));
});
function sideMarks() {
  const view = viewOf(), here = F.cwd.toLowerCase();
  for (const a of $$('#side [data-view]')) a.classList.toggle('on', a.dataset.view === view);
  for (const a of $$('#side [data-place]')) a.classList.toggle('on', view === 'files' && !a.dataset.file && a.dataset.place.toLowerCase() === here);
}
function storage() {
  const box = $('#storage');
  box.hidden = F.free < 0;
  if (box.hidden) return;
  $('#free').textContent = size(F.free) + ' free';
  const known = F.total > 0;
  $('#meter').hidden = $('#used').hidden = !known;
  if (!known) return;
  const used = (F.total - F.free) / F.total;
  $('#meter').firstChild.style.width = (used * 100).toFixed(1) + '%';
  $('#meter').classList.toggle('full', used > .92);
  $('#used').textContent = size(F.total - F.free) + ' of ' + size(F.total) + ' used';
}

// --- Links ------------------------------------------------------------------------
function linksIn(text) { return [...new Set((text.match(/https?:\/\/[^\s<>"'()]+/gi) || []).map(l => l.replace(/[.,;]+$/, '')))]; }
function linkName(l) { try { return decodeURIComponent(base(new URL(l).pathname)) || new URL(l).host; } catch (e) { return l; } }
$('#sendform').noValidate = true;
$('#linkbox').addEventListener('input', () => {
  const n = linksIn($('#linkbox').value).length;
  $('#linkcount').textContent = n ? plural(n, 'link') : '';
});
$('#sendform').addEventListener('submit', async e => {
  e.preventDefault();
  const box = $('#linkbox'), links = linksIn(box.value), b = $('#sendbtn');
  if (!links.length) {
    box.classList.remove('shake');
    void box.offsetWidth;
    box.classList.add('shake');
    toast('There are no http or https links in that.', 'bad');
    return;
  }
  // remote::MaxSentLinks
  if (links.length > 20) {
    toast('Send at most 20 links at once.', 'bad');
    return;
  }
  b.classList.add('busy');
  try {
    const j = await api('POST', '/send', {links: box.value});
    const li = document.createElement('li');
    li.className = 'wait';
    li.innerHTML = svg('link') + '<div><b></b><span>Sent to the 3DS.</span></div>';
    li.querySelector('b').textContent = plural(j.count, 'link') + ': ' + links.slice(0, 2).map(linkName).join(', ') + (links.length > 2 ? ' and ' + (links.length - 2) + ' more' : '');
    $('#sent').prepend(li);
    $('#sentbox').hidden = false;
    S.expect.push(li);
    box.value = '';
    box.dispatchEvent(new Event('input'));
    S.state = 'choosing';
    S.text = 'Waiting for you to choose Download on the 3DS.';
    showStatus();
    poll();
  } catch (err) { fail(err); } finally { b.classList.remove('busy'); }
});

// --- Files ------------------------------------------------------------------------
// sel holds the selected paths, anchor where a Shift range starts, focus the
// item the arrow keys move from, pick a path to select once its folder is
// shown, clip what Cut or Copy took.
const F = {cwd: '/', entries: [], free: -1, total: -1, sel: new Set(), anchor: '', focus: '', pick: '', token: 0, renders: 0, fresh: new Set(),
  nodes: new Map(), order: [], picking: false, clip: null,
  view: store.get('view', 'grid'), sort: store.get('sortby', {by: 'name', desc: false}), hidden: store.get('hidden', false)};
if (!['name', 'kind', 'size', 'time'].includes(F.sort.by)) F.sort = {by: 'name', desc: false};
if (iOS) $('#pickdir').remove();
function gate(state) {
  S.access = state;
  if (!onFiles()) return;
  document.body.classList.add('gated');
  $('#files').classList.add('gated');
  $('#browser').hidden = $('#pathbar').hidden = true;
  const g = $('#gate');
  g.hidden = false;
  const card = (cls, icon, title, rest) => '<div class="gatecard' + cls + '"><div class="art"><span>' + svg(icon) + '</span></div><h2>' + title + '</h2>' + rest + '</div>';
  if (state === 'pending') {
    g.innerHTML = card('', 'lock', 'Choose Allow on the 3DS', '<dl class="facts"' + (S.you ? '' : ' hidden') + '><dt>Address</dt><dd class="mono" id="gateyou">' + esc(S.you) + '</dd></dl>');
    poll();
  } else if (state === 'denied') {
    g.innerHTML = card(' no', 'lock', 'Not allowed', '<p>To ask again, leave Receive on the 3DS and open it again.</p>');
  } else if (state === 'off') {
    g.innerHTML = card(' no', 'lock', 'Files are not shared', '');
  } else {
    g.innerHTML = card('', 'console', 'Connecting to the 3DS', '');
  }
}
function ungate() {
  document.body.classList.remove('gated');
  $('#files').classList.remove('gated');
  $('#gate').hidden = true;
  $('#browser').hidden = $('#pathbar').hidden = false;
}
function skeleton() {
  const box = $('#items');
  box.classList.remove('dim');
  box.innerHTML = '<span class="skel"></span>'.repeat(F.view === 'grid' ? 10 : 8);
}
function loading(on) {
  $('#loadline').classList.toggle('on', on);
  $('#items').classList.toggle('dim', on && !!F.entries.length);
}
async function load(path, quietly, priority) {
  const token = ++F.token, moved = path !== F.cwd;
  F.cwd = path;
  if (moved) { F.sel.clear(); F.anchor = F.focus = ''; F.picking = false; $('#filter').value = ''; }
  titles();
  sideMarks();
  loading(true);
  if ($('#browser').hidden && !quietly && S.access !== 'pending') gate('connecting');
  const slow = setTimeout(() => { if (token === F.token && moved && !quietly) skeleton(); }, 180);
  try {
    const j = await api('GET', '/api/list?path=' + enc(path) + (F.hidden ? '&hidden=1' : ''), undefined, priority !== undefined ? priority : quietly ? 1 : 0);
    if (token !== F.token) return;
    Object.assign(F, {entries: j.entries, free: j.free, total: j.total});
    S.access = 'allowed';
    ungate();
    render(moved || !quietly);
    showStatus();
    places();
    if (j.times === false) fillTimes(path, token);
  } catch (e) {
    if (token !== F.token || e.quiet) return;
    if (S.offline) { if (!$('#browser').hidden) render(false); return; }
    if (path !== '/') { toast(e.message, 'bad'); go(parent(path)); } else fail(e);
  } finally { clearTimeout(slow); if (token === F.token) loading(false); }
}
// When each of names in the folder at path changed, for folders too big to
// list with times: the 3DS takes about a tenth of a second for each.
async function timesOf(path, names, priority, each) {
  const out = new Map();
  for (let i = 0; i < names.length; i += 50) {
    const part = names.slice(i, i + 50);
    const j = await api('POST', '/api/times', {path, names: part.join('\n')}, priority, {retry: true});
    for (const n of part) out.set(n, j.times[n] || 0);
    if (each && each(out, part) === false) break;
  }
  return out;
}
// Fills in the dates of a big folder after it is shown, while nothing more
// urgent waits.
async function fillTimes(path, token) {
  const named = new Map(F.entries.map(e => [e.name, e]));
  try {
    await timesOf(path, [...named.keys()], 3, (got, part) => {
      if (token !== F.token) return false;
      for (const n of part) {
        named.get(n).time = got.get(n);
        const node = F.nodes.get(join(path, n));
        if (node) node.querySelector('.dt').textContent = when(got.get(n));
      }
    });
  } catch (e) {}
  if (token === F.token && F.sort.by === 'time') render(false);
}
// Folders first, then by the chosen column. Sizes and dates go biggest and
// newest first; desc turns any order around.
function sorted() {
  const {by, desc} = F.sort, turn = desc ? -1 : 1;
  return F.entries.slice().sort((a, b) => a.dir !== b.dir ? (a.dir ? -1 : 1) : turn * (
    by === 'size' && !a.dir ? b.size - a.size || byName(a, b)
    : by === 'time' ? b.time - a.time || byName(a, b)
    : by === 'kind' && !a.dir ? kindOf(a).localeCompare(kindOf(b)) || byName(a, b)
    : byName(a, b)));
}
function shown() {
  const q = $('#filter').value.trim().toLowerCase();
  return sorted().filter(e => !q || e.name.toLowerCase().includes(q));
}
function highlight(name) {
  const q = $('#filter').value.trim().toLowerCase();
  const at = q ? name.toLowerCase().indexOf(q) : -1;
  if (at < 0) return esc(name);
  return esc(name.slice(0, at)) + '<mark>' + esc(name.slice(at, at + q.length)) + '</mark>' + esc(name.slice(at + q.length));
}
const entryOf = path => { const node = F.nodes.get(path); return node ? node.entry : null; };
function titles() {
  const name = folderName(F.cwd);
  $('#title').textContent = name;
  $('#filter').placeholder = 'Search ' + name;
  $('#back').disabled = N.at <= 0;
  $('#fwd').disabled = N.at >= N.stack.length - 1;
}
let watcher = null;
function render(animate) {
  F.renders++;
  titles();
  storage();
  const box = $('#items'), list = shown();
  box.className = 'items ' + F.view;
  box.textContent = '';
  F.nodes = new Map();
  F.order = list.map(e => join(F.cwd, e.name));
  if (watcher) watcher.disconnect();
  watcher = 'IntersectionObserver' in window ? new IntersectionObserver(seen, {root: matchMedia('(max-width: 759px)').matches ? null : $('#pane'), rootMargin: '120px'}) : null;
  if (!list.length) box.innerHTML = '<div class="empty">' + svg(F.entries.length ? 'search' : 'folder') + (F.entries.length ? 'Nothing matches' : 'This folder is empty') + '</div>';
  list.forEach((e, n) => {
    const path = join(F.cwd, e.name), node = document.createElement('div');
    node.className = 'item' + (animate ? ' anim' : '') + (F.fresh.has(path) ? ' fresh' : '');
    node.style.setProperty('--i', Math.min(n, 18));
    node.setAttribute('role', 'option');
    node.tabIndex = -1;
    node.dataset.path = path;
    if (e.dir) node.dataset.drop = path;
    node.draggable = fine.matches;
    node.innerHTML = '<input type="checkbox" class="pick" tabindex="-1" aria-label="Select ' + esc(e.name) + '"><span class="main">' + glyph(e) + '<span class="nm">' + highlight(e.name) +
      '</span></span><span class="kd">' + esc(kindOf(e)) + '</span><span class="sz">' + (e.dir ? '' : esc(size(e.size))) + '</span><span class="dt">' + esc(when(e.time)) +
      '</span><span class="mt">' + esc(e.dir ? 'Folder' : size(e.size)) + '</span>';
    node.entry = e;
    box.append(node);
    F.nodes.set(path, node);
    if (!e.dir && TITLED.has(ext(e.name))) { if (watcher) watcher.observe(node); else icon(node); }
  });
  F.fresh.clear();
  for (const p of F.sel) if (!F.nodes.has(p)) F.sel.delete(p);
  if (F.pick && F.nodes.has(F.pick)) { F.sel = new Set([F.pick]); F.anchor = F.focus = F.pick; reveal(F.nodes.get(F.pick)); }
  F.pick = '';
  $('#lhead').hidden = F.view !== 'list';
  for (const b of $$('#lhead [data-sort]')) {
    const on = b.dataset.sort === F.sort.by, upward = (F.sort.by === 'size' || F.sort.by === 'time') === F.sort.desc;
    b.classList.toggle('on', on);
    b.innerHTML = esc(b.textContent) + (on ? svg(upward ? 'upward' : 'down') : '');
  }
  $('#asgrid').setAttribute('aria-pressed', F.view === 'grid');
  $('#aslist').setAttribute('aria-pressed', F.view === 'list');
  $('#sortbtn').innerHTML = esc(SORTS[F.sort.by]) + svg('down');
  paint();
}
function reveal(node) { node.scrollIntoView({block: 'nearest', behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth'}); }
// Real icons for games and homebrew, fetched once each when they scroll in.
const icons = new Map();
function seen(changes) { for (const c of changes) if (c.isIntersecting) { watcher.unobserve(c.target); icon(c.target); } }
function iconFor(path, time) {
  const key = path + '@' + time;
  if (!icons.has(key)) {
    const token = F.token;
    icons.set(key, run(async () => {
      if (token !== F.token) { icons.delete(key); return null; }
      try { const r = await fetch('/api/icon?path=' + enc(path) + '&t=' + time); return r.ok ? URL.createObjectURL(await r.blob()) : null; } catch (e) { return null; }
    }, 3, 'icon'));
  }
  return icons.get(key);
}
async function icon(node, into = node) {
  const url = await iconFor(node.dataset.path, node.entry.time);
  const tile = into.querySelector('.app');
  if (url && tile) { tile.classList.add('real'); tile.innerHTML = '<img alt="" src="' + url + '">'; }
}

// --- Selection ------------------------------------------------------------------------
function paint() {
  for (const [path, node] of F.nodes) {
    const on = F.sel.has(path);
    node.classList.toggle('on', on);
    node.setAttribute('aria-selected', on);
    node.firstChild.checked = on;
    node.tabIndex = path === (F.focus || F.order[0]) ? 0 : -1;
  }
  if (!F.sel.size) F.picking = false;
  $('#items').classList.toggle('picking', F.picking);
  const n = F.sel.size, all = $('#all');
  all.checked = n > 0 && n >= F.order.length;
  all.indeterminate = n > 0 && n < F.order.length;
  const total = F.entries.length, listed = F.order.length;
  $('#count').textContent = (listed === total ? plural(total, 'item') : listed + ' of ' + plural(total, 'item')) + (n ? ', ' + n + ' selected' : '');
  crumbs();
  detail();
  selbar();
}
function crumbs() {
  const nav = $('#crumbs'), sep = svg('right');
  let html = '', p = '';
  const parts = F.cwd.split('/').filter(Boolean);
  const link = (label, to) => '<a href="#files:' + enc(to) + '" data-drop="' + esc(to) + '">' + esc(label) + '</a>';
  html = link('SD Card', '/');
  for (const part of parts) { p += '/' + part; html += sep + link(part, p); }
  if (F.sel.size === 1) html += sep + '<span>' + esc(base([...F.sel][0])) + '</span>';
  nav.innerHTML = html;
}
function setSel(paths) { F.sel = new Set(paths); paint(); }
function selectOnly(path) { F.anchor = F.focus = path; setSel([path]); focusItem(path); }
function toggleSel(path) {
  const sel = new Set(F.sel);
  if (sel.has(path)) sel.delete(path); else sel.add(path);
  F.anchor = F.focus = path;
  setSel(sel);
}
// From the anchor to path; with add, on top of what is selected.
function rangeSel(path, add) {
  const a = F.order.indexOf(F.anchor || path), b = F.order.indexOf(path);
  const range = a < 0 || b < 0 ? [path] : F.order.slice(Math.min(a, b), Math.max(a, b) + 1);
  F.focus = path;
  setSel(add ? [...F.sel, ...range] : range);
  focusItem(path);
}
function focusItem(path) {
  const node = F.nodes.get(path);
  if (!node) return;
  F.focus = path;
  for (const n of F.nodes.values()) n.tabIndex = n === node ? 0 : -1;
  node.focus({preventScroll: true});
  node.scrollIntoView({block: 'nearest'});
}
// Opens what double clicking or Enter means for an entry: a folder, a
// picture, text to edit, a song or video; anything else shows its details.
function openItem(path) {
  const e = entryOf(path);
  if (!e) return;
  const x = ext(e.name);
  if (e.dir) go(path);
  else if (PICTURES.has(x)) view(path);
  else if (TEXT.has(x) && e.size <= 1048576) edit(path);
  else if (MEDIA.has(x)) window.open(fileUrl(path), '_blank');
  else if (!wide.matches) info([path]);
}
// What can be done with the selected paths, or with the folder when there
// are none. The first marked main is the big button.
function actionsFor(paths) {
  const list = [], add = (label, icon, fn, more = {}) => list.push({label, icon, fn, ...more});
  if (!paths.length) {
    add('Upload Files', 'upload', () => $('#pick').click(), {main: true});
    if (!iOS) add('Upload Folder', 'folder', () => $('#pickdir').click());
    add('New Folder', 'folderplus', mkdir);
    if (F.clip) add(F.clip.cut ? 'Move Here' : 'Paste Here', 'paste', paste);
    return list;
  }
  const one = paths.length === 1, p = paths[0], e = entryOf(p) || {name: base(p), dir: false, size: 0}, x = ext(e.name);
  const cias = paths.filter(q => CIAS.has(ext(q)) && !(entryOf(q) || {}).dir);
  if (one && e.dir) add('Open', 'folder', () => go(p), {main: true});
  else if (one) {
    if (CIAS.has(x)) add('Install on 3DS', 'install', () => install(paths), {main: true});
    else if (PICTURES.has(x)) add('View', 'eye', () => view(p), {main: true});
    else if (TEXT.has(x) && e.size <= 1048576) add('Edit', 'edit', () => edit(p), {main: true});
    else if (MEDIA.has(x)) add('Open', 'eye', () => window.open(fileUrl(p), '_blank'), {main: true});
    add('Download', 'download', () => download(p, e), {main: !list.length, id: 'download'});
  } else if (cias.length) add(cias.length === paths.length ? 'Install on 3DS' : 'Install ' + plural(cias.length, 'CIA'), 'install', () => install(cias), {main: true});
  if (one) add('Rename', 'rename', () => rename(p));
  add('Move To...', 'move', () => pickFolder(paths, false), {id: 'move'});
  add('Copy To...', 'copy', () => pickFolder(paths, true), {id: 'copyto'});
  if (one) add('Copy Path', 'link', () => copyText(p));
  add('Delete', 'trash', () => remove(paths), {red: true, id: 'delete'});
  return list;
}
// The details of the selection, or of the folder: in the column on the
// right, or in a sheet where there is no room for it.
function fillPanel(box, paths, close) {
  const key = paths.join('\n') + '|' + F.cwd + '|' + F.renders + '|' + !!F.clip;
  if (box.dataset.key === key) return;
  box.dataset.key = key;
  box.textContent = '';
  const es = paths.map(entryOf).filter(Boolean), one = es.length === 1, e = es[0];
  const pv = document.createElement('div');
  pv.className = 'pv';
  let facts = [];
  if (!paths.length) {
    pv.innerHTML = glyph({dir: true}) + '<h2>' + esc(folderName(F.cwd)) + '</h2>';
    facts = [['Items', String(F.entries.length)]];
  } else if (one) {
    pv.innerHTML = glyph(e) + '<h2>' + esc(e.name) + '</h2><span class="sub">' + esc(kindOf(e)) + '</span>';
    facts = e.dir ? [] : [['Size', size(e.size), e.size >= 1024 ? e.size.toLocaleString() + ' bytes' : '']];
    if (e.time) facts.push(['Changed', when(e.time)]);
  } else {
    pv.innerHTML = '<div class="stack">' + es.slice(0, 3).reverse().map(glyph).join('') + '</div><h2>' + plural(es.length, 'item') + '</h2>';
    const files = es.filter(x => !x.dir), folders = es.length - files.length;
    if (files.length) facts.push(['Files', files.length + ', ' + size(files.reduce((n, x) => n + x.size, 0))]);
    if (folders) facts.push(['Folders', String(folders)]);
    const kinds = new Set(es.map(kindOf));
    if (kinds.size === 1) facts.push(['Kind', [...kinds][0]]);
  }
  box.append(pv);
  const dl = document.createElement('dl');
  dl.className = 'facts';
  const fact = (k, v, title, mono) => '<dt>' + esc(k) + '</dt><dd' + (mono ? ' class="mono"' : '') + (title ? ' title="' + esc(title) + '"' : '') + '>' + esc(v) + '</dd>';
  dl.innerHTML = facts.map(f => fact(...f)).join('');
  if (facts.length) box.append(dl);
  const acts = document.createElement('div'), two = document.createElement('div');
  acts.className = 'acts';
  two.className = 'two';
  for (const a of actionsFor(paths)) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'btn' + (a.main ? ' primary' : '') + (a.red ? ' red' : '');
    b.innerHTML = a.main ? svg(a.icon) + esc(a.label) : esc(a.label);
    b.onclick = () => { if (close) close(); a.fn(); };
    (a.main ? acts : two).append(b);
  }
  acts.append(two);
  box.append(acts);
  if (one && !e.dir && TITLED.has(ext(e.name))) {
    icon(F.nodes.get(paths[0]), pv);
    titleFacts(paths[0], e, box, dl, key);
  }
}
// The name, publisher, title ID and version inside a CIA, 3DSX or game.
const META = new Map();
async function titleFacts(path, e, box, dl, key) {
  const id = path + '@' + e.time;
  if (!META.has(id)) META.set(id, api('GET', '/api/meta?path=' + enc(path), undefined, 1).catch(() => null));
  if (!dl.isConnected) box.insertBefore(dl, box.lastChild);
  dl.insertAdjacentHTML('afterbegin', '<span class="skel"></span><span class="skel" style="width:60%"></span>');
  const m = await META.get(id);
  if (box.dataset.key !== key) return;
  for (const s of dl.querySelectorAll('.skel')) s.remove();
  if (!m) return;
  const rows = [['Title', m.name], ['Publisher', m.publisher], ['Product', m.product], ['Title ID', m.titleId, '', true], ['Version', m.versionText], ['Note', m.note]].filter(r => r[1]);
  dl.insertAdjacentHTML('afterbegin', rows.map(([k, v, t, mono]) => '<dt>' + esc(k) + '</dt><dd' + (mono ? ' class="mono"' : '') + '>' + esc(v) + '</dd>').join(''));
  if (m.name) box.querySelector('.sub').textContent = m.name + (m.publisher ? ', ' + m.publisher : '');
}
function detail() { if (wide.matches && browsing()) fillPanel($('#detail'), [...F.sel]); }
function info(paths) {
  const d = $('#sheet');
  d.innerHTML = '<button class="close" aria-label="Close">' + svg('x') + '</button><div class="panel"></div>';
  d.querySelector('.close').onclick = () => d.close();
  d.onclick = ev => { if (ev.target === d) d.close(); };
  fillPanel(d.querySelector('.panel'), paths, () => d.close());
  d.showModal();
}
// Without the details column, the selection's actions sit in a bar at the
// bottom: the main one, then Move, Copy, Download, Delete and the rest.
function selbar() {
  const bar = $('#selbar'), paths = [...F.sel];
  bar.hidden = !paths.length || !browsing();
  if (bar.hidden) return;
  bar.innerHTML = '<b>' + paths.length + ' selected</b><span class="acts2"></span>';
  const row = bar.lastChild, acts = actionsFor(paths), button = (a, iconOnly, into = row) => {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'btn' + (iconOnly ? ' icon' : ' primary') + (a.red ? ' red' : '');
    b.innerHTML = iconOnly ? svg(a.icon) : esc(a.label);
    b.setAttribute('aria-label', a.label);
    b.title = a.label;
    b.onclick = () => a.fn();
    into.append(b);
    return b;
  };
  const main = acts.find(a => a.main && a.id !== 'download');
  if (main) button(main, false);
  for (const id of ['move', 'copyto', 'download', 'delete']) { const a = acts.find(x => x.id === id); if (a) button(a, true); }
  button({label: 'Details', icon: 'info', fn: () => info(paths)}, true, bar).classList.add('plain');
  button({label: 'Clear selection', icon: 'x', fn: () => setSel([])}, true, bar).classList.add('plain');
}
wide.addEventListener('change', () => { $('#detail').dataset.key = ''; paint(); progress(); });

// --- Pointer, touch and keys -------------------------------------------------------
// Mice: click selects, Cmd or Ctrl adds, Shift adds a range, double click
// opens, a drag from an empty spot draws a box. Touch: a tap opens a folder
// and selects a file, a tap on the selected file opens it, a long press
// starts picking several.
let pointer = 'mouse', press = null, swallow = false;
const items = $('#items');
items.addEventListener('pointerdown', e => {
  pointer = e.pointerType;
  swallow = false;
  const node = e.target.closest('.item');
  if (pointer === 'mouse' || !node) return;
  press = {x: e.clientX, y: e.clientY, timer: setTimeout(() => {
    press = null;
    swallow = true;
    // Picking starts with the pressed item, not with a file a tap selected.
    if (F.picking) toggleSel(node.dataset.path);
    else { F.picking = true; F.anchor = F.focus = node.dataset.path; setSel([node.dataset.path]); }
    if (navigator.vibrate) navigator.vibrate(8);
  }, 450)};
});
addEventListener('pointermove', e => { if (press && Math.hypot(e.clientX - press.x, e.clientY - press.y) > 10) { clearTimeout(press.timer); press = null; } });
for (const type of ['pointerup', 'pointercancel']) addEventListener(type, () => { if (press) { clearTimeout(press.timer); press = null; } });
items.addEventListener('click', e => {
  const node = e.target.closest('.item');
  if (!node || swallow) { swallow = false; return; }
  const path = node.dataset.path;
  if (e.target.classList.contains('pick')) { e.preventDefault(); if (e.shiftKey) rangeSel(path, true); else { F.picking = !fine.matches; toggleSel(path); } return; }
  if (e.shiftKey) return rangeSel(path, e.metaKey || e.ctrlKey);
  if (e.metaKey || e.ctrlKey) return toggleSel(path);
  if (pointer === 'mouse') return selectOnly(path);
  if (F.picking) return toggleSel(path);
  if (node.entry.dir || (F.sel.size === 1 && F.sel.has(path))) return openItem(path);
  selectOnly(path);
});
items.addEventListener('dblclick', e => { const node = e.target.closest('.item'); if (node && pointer === 'mouse') openItem(node.dataset.path); });
$('#pane').addEventListener('contextmenu', e => {
  e.preventDefault();
  if (pointer !== 'mouse') return;
  const node = e.target.closest('.item');
  if (node && !F.sel.has(node.dataset.path)) selectOnly(node.dataset.path);
  if (!node) setSel([]);
  const acts = actionsFor(node ? [...F.sel] : []);
  popup({x: e.clientX, y: e.clientY}, acts.flatMap(a => (a.red ? ['-'] : []).concat(a)));
});
// The box: items it touches are selected, on top of those already selected
// when Shift, Cmd or Ctrl is held.
$('#pane').addEventListener('pointerdown', e => {
  if (e.pointerType !== 'mouse' || e.button !== 0 || e.target.closest('.item,.lhead,button,input,a')) return;
  const pane = $('#pane'), pr = pane.getBoundingClientRect();
  if (e.clientX > pr.left + pane.clientWidth || e.clientY > pr.top + pane.clientHeight) return;
  const start = items.getBoundingClientRect(), x0 = e.clientX - start.left, y0 = e.clientY - start.top;
  const keep = e.shiftKey || e.metaKey || e.ctrlKey ? new Set(F.sel) : new Set();
  const boxes = [...F.nodes].map(([path, n]) => [path, n.offsetLeft, n.offsetTop, n.offsetWidth, n.offsetHeight]);
  let mark = null;
  const moveTo = ev => {
    const now = items.getBoundingClientRect(), x1 = ev.clientX - now.left, y1 = ev.clientY - now.top;
    if (!mark && Math.hypot(x1 - x0, y1 - y0) < 5) return;
    if (!mark) { mark = document.createElement('div'); mark.className = 'marquee'; items.append(mark); }
    const l = Math.min(x0, x1), t = Math.min(y0, y1), w = Math.abs(x1 - x0), h = Math.abs(y1 - y0);
    Object.assign(mark.style, {left: l + 'px', top: t + 'px', width: w + 'px', height: h + 'px'});
    const hit = new Set(keep);
    for (const [path, bl, bt, bw, bh] of boxes) if (bl < l + w && bl + bw > l && bt < t + h && bt + bh > t) hit.add(path);
    F.sel = hit;
    for (const [path, n] of F.nodes) n.classList.toggle('on', hit.has(path));
  };
  const end = () => {
    removeEventListener('pointermove', moveTo);
    removeEventListener('pointerup', end);
    if (mark) { mark.remove(); paint(); } else if (!keep.size) setSel([]);
  };
  addEventListener('pointermove', moveTo);
  addEventListener('pointerup', end);
});
// Arrow keys move through the items, by rows in the icon view.
function step(key, extend) {
  const order = F.order;
  if (!order.length) return;
  let at = order.indexOf(F.focus);
  let cols = 1;
  if (F.view === 'grid') { const nodes = [...F.nodes.values()]; while (cols < nodes.length && nodes[cols].offsetTop === nodes[0].offsetTop) cols++; }
  const by = {ArrowLeft: F.view === 'grid' ? -1 : 0, ArrowRight: F.view === 'grid' ? 1 : 0, ArrowUp: -cols, ArrowDown: cols}[key];
  const next = key === 'Home' ? 0 : key === 'End' ? order.length - 1 : at < 0 ? 0 : Math.max(0, Math.min(order.length - 1, at + by));
  if (extend) rangeSel(order[next], false); else selectOnly(order[next]);
}
addEventListener('keydown', e => {
  if (menuOpen) {
    const buttons = [...menuOpen.querySelectorAll('button')], at = buttons.indexOf(document.activeElement);
    if (e.key === 'Escape') { e.preventDefault(); closeMenu(); }
    else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); buttons[(at + (e.key === 'ArrowDown' ? 1 : buttons.length - 1)) % buttons.length].focus(); }
    return;
  }
  if (!browsing() || document.querySelector('dialog[open]')) return;
  const el = document.activeElement, mod = e.metaKey || e.ctrlKey;
  const typing = /INPUT|TEXTAREA|SELECT/.test(el.tagName) && el.type !== 'checkbox';
  const onItems = el === document.body || items.contains(el) || el.id === 'pane';
  if (e.key === 'Escape') {
    if (el.id === 'filter') { $('#filter').value = ''; render(false); el.blur(); } else if (F.sel.size) setSel([]);
    return;
  }
  if (typing) return;
  const sel = [...F.sel], k = e.key.toLowerCase();
  if (e.key === '/' || (mod && k === 'f')) { e.preventDefault(); $('#filter').focus(); }
  else if (mod && e.shiftKey && e.key === '.') { e.preventDefault(); showHidden(!F.hidden); }
  else if (mod && k === 'a') { e.preventDefault(); setSel(F.order); }
  else if (mod && (k === 'c' || k === 'x') && sel.length && !String(getSelection())) { e.preventDefault(); clip(sel, k === 'x'); }
  else if (mod && k === 'v' && F.clip) { e.preventDefault(); paste(); }
  else if (sel.length && (e.key === 'Delete' || (mod && e.key === 'Backspace'))) { e.preventDefault(); remove(sel); }
  else if (e.key === 'Backspace' || ((e.altKey || mod) && e.key === 'ArrowUp')) { e.preventDefault(); if (F.cwd !== '/') go(parent(F.cwd)); }
  else if (onItems && F.focus && (e.key === 'Enter' || (mod && e.key === 'ArrowDown'))) { e.preventDefault(); openItem(F.focus); }
  else if (e.key === 'F2' && sel.length === 1) { e.preventDefault(); rename(sel[0]); }
  else if (onItems && ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(e.key)) { e.preventDefault(); step(e.key, e.shiftKey); }
  else if (onItems && e.key === ' ' && F.focus) { e.preventDefault(); toggleSel(F.focus); }
  else if (k === 'u' && !mod) $('#pick').click();
});

// --- The toolbar -------------------------------------------------------------------
const SORTS = {name: 'Name', kind: 'Kind', size: 'Size', time: 'Date'};
function sortBy(by) {
  F.sort = {by, desc: F.sort.by === by ? !F.sort.desc : false};
  store.set('sortby', F.sort);
  render(true);
}
function showHidden(on) { F.hidden = on; store.set('hidden', on); load(F.cwd, true); }
function setView(v) { F.view = v; store.set('view', v); render(true); }
$('#sortbtn').onclick = e => popup(e.currentTarget, Object.keys(SORTS).map(by => ({label: SORTS[by], checked: F.sort.by === by, fn: () => sortBy(by)}))
  .concat('-', {label: 'Show Hidden Files', checked: F.hidden, fn: () => showHidden(!F.hidden)}));
$('#lhead').addEventListener('click', e => { const b = e.target.closest('[data-sort]'); if (b) sortBy(b.dataset.sort); });
$('#all').onchange = e => setSel(e.target.checked ? F.order : []);
$('#asgrid').onclick = () => setView('grid');
$('#aslist').onclick = () => setView('list');
$('#filter').oninput = () => render(false);
$('#mkdir').onclick = () => mkdir();

// --- Doing things with files ----------------------------------------------------------
function download(path, e) {
  if (e.size > 64 * 1048576) toast('Downloading ' + e.name + '. The 3DS answers nothing else until it finishes.');
  const a = document.createElement('a');
  a.href = fileUrl(path, true);
  a.download = e.name;
  document.body.append(a);
  a.click();
  a.remove();
}
function copyText(text) {
  const done = () => toast('Copied ' + text, 'good');
  const fallback = () => {
    const t = document.createElement('textarea');
    t.value = text;
    t.style.position = 'fixed';
    t.style.opacity = '0';
    document.body.append(t);
    t.select();
    try { document.execCommand('copy'); done(); } catch (e) { toast(text); }
    t.remove();
  };
  if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(text).then(done, fallback);
  else fallback();
}
async function rename(path) {
  const r = await ask('Rename', null, {input: base(path), select: true, buttons: ['Cancel', 'Rename'], warn: risk([path]), action: async name => {
    name = name.trim();
    if (!name || name === base(path)) return false;
    await api('POST', '/api/move', {from: path, to: join(parent(path), name)});
    return name;
  }});
  if (typeof r !== 'string') return;
  F.pick = join(parent(path), r);
  F.fresh.add(F.pick);
  toast('Renamed to ' + r, 'good');
  load(F.cwd, true);
}
async function mkdir(into = F.cwd) {
  let made = '';
  const r = await ask('New Folder', null, {input: '', buttons: ['Cancel', 'Create'], action: async name => {
    name = name.trim();
    if (!name) throw new Error('Type a name for the folder.');
    made = join(into, name);
    await api('POST', '/api/folder', {path: made}, 0, {retry: true});
  }});
  if (r !== true) return '';
  if (into === F.cwd) { F.pick = made; F.fresh.add(made); load(F.cwd, true); }
  return made;
}
async function remove(paths) {
  const ok = await ask('Delete ' + (paths.length > 1 ? paths.length + ' items' : '"' + base(paths[0]) + '"') + '?', 'Deleted files and folders cannot be brought back.',
    {buttons: ['Cancel', 'Delete'], danger: true, warn: risk(paths), action: async () => { for (const p of paths) await api('POST', '/api/delete', {path: p}, 0, {retry: true}); }});
  if (ok !== true) return;
  for (const p of paths) { const node = F.nodes.get(p); if (node) node.classList.add('gone'); }
  setSel([]);
  toast('Deleted ' + what(paths), 'good');
  setTimeout(() => load(F.cwd, true), 280);
}
async function install(paths) {
  const cias = paths.filter(p => CIAS.has(ext(p)));
  if (!cias.length) return;
  try {
    await api('POST', '/api/install', {paths: cias.join('\n')});
    S.expect.push(null);
    S.state = 'choosing';
    S.text = 'Waiting for you to choose Install on the 3DS.';
    showStatus();
    toast('Sent to the 3DS. Choose Install on the console.');
    poll();
  } catch (e) { fail(e); }
}
// Cut and Copy keep the entries too, so the clash question can show them
// after the folder they came from is left.
function clip(paths, cut) {
  F.clip = {cut, paths, entries: new Map(paths.map(p => [p, entryOf(p)]))};
  toast((cut ? 'Cut ' : 'Copied ') + what(paths));
  $('#detail').dataset.key = '';
  detail();
}
async function paste() {
  const c = F.clip;
  if (!c) return;
  if (await transfer(c.paths, F.cwd, !c.cut, c.entries) && c.cut) { F.clip = null; detail(); }
}

// --- Move To and Copy To -------------------------------------------------------------
// Folders in columns, as in Finder's column view, starting at the open one.
const P = {paths: [], cols: [], dest: '/'};
async function pickFolder(paths, copying) {
  Object.assign(P, {paths, cols: [], dest: F.cwd});
  $('#ptitle').textContent = (copying ? 'Copy ' : 'Move ') + what(paths);
  $('#pmove').className = 'btn' + (copying ? '' : ' primary');
  $('#pcopy').className = 'btn' + (copying ? ' primary' : '');
  $('#pcols').textContent = '';
  $('#picker').showModal();
  const chain = ['/'];
  for (const part of F.cwd.split('/').filter(Boolean)) chain.push(join(chain[chain.length - 1], part));
  aim(F.cwd);
  for (let i = 0; i < chain.length && (!i || P.cols[i - 1] === chain[i - 1]); i++) await column(i, chain[i], chain[i + 1]);
}
async function column(i, folder, chosen) {
  P.cols.length = i;
  P.cols[i] = folder;
  const box = $('#pcols');
  while (box.children.length > i) box.lastChild.remove();
  const col = document.createElement('div');
  col.className = 'col';
  col.setAttribute('role', 'listbox');
  col.innerHTML = '<span class="spin"></span>';
  box.append(col);
  box.scrollLeft = box.scrollWidth;
  let names;
  try {
    const j = await api('GET', '/api/list?times=0&path=' + enc(folder) + (F.hidden ? '&hidden=1' : ''));
    names = j.entries.filter(e => e.dir).sort(byName).map(e => e.name);
  } catch (e) { if (!e.quiet) col.innerHTML = '<span class="err">' + esc(e.message) + '</span>'; return; }
  if (!col.isConnected) return;
  col.textContent = '';
  if (!names.length) col.innerHTML = '<span class="none">No folders</span>';
  for (const name of names) {
    const path = join(folder, name), b = document.createElement('button');
    b.type = 'button';
    b.className = 'fold' + (path === chosen ? ' on' : '');
    b.dataset.path = path;
    b.disabled = P.paths.some(p => inside(path, p));
    b.innerHTML = FOLDER + '<span>' + esc(name) + '</span>' + svg('right');
    b.onclick = () => {
      for (const o of col.querySelectorAll('.on')) o.classList.remove('on');
      b.classList.add('on');
      aim(path);
      column(i + 1, path);
    };
    col.append(b);
  }
  box.scrollLeft = box.scrollWidth;
}
function aim(path) {
  P.dest = path;
  const parts = path.split('/').filter(Boolean), sep = svg('right');
  $('#pcrumbs').innerHTML = '<span>SD Card</span>' + parts.map(p => sep + '<span>' + esc(p) + '</span>').join('');
  const into = P.paths.some(p => inside(path, p));
  $('#pmove').disabled = into || P.paths.every(p => parent(p) === path);
  $('#pcopy').disabled = into;
}
$('#pcancel').onclick = () => $('#picker').close();
$('#pmove').onclick = () => { $('#picker').close(); transfer(P.paths, P.dest, false); };
$('#pcopy').onclick = () => { $('#picker').close(); transfer(P.paths, P.dest, true); };
$('#pmkdir').onclick = async () => {
  const at = P.cols.indexOf(P.dest), made = await mkdir(P.dest);
  if (!made || at < 0) return;
  await column(at, P.dest, made);
  aim(made);
  column(at + 1, made);
};
// Moves or copies paths into the folder dest, asking once about names
// already there: Skip leaves them, Keep Both gives the new ones the next
// free name, Replace puts them in place and merges folders. Copying into
// the folder something is in keeps both. True when it got through.
async function transfer(paths, dest, copying, known = new Map()) {
  const verb = copying ? 'Copy' : 'Move';
  const work = paths.filter(p => copying || parent(p) !== dest);
  if (!work.length) { toast('Already in ' + folderName(dest) + '.'); return false; }
  const bad = work.find(p => inside(dest, p));
  if (bad) { toast(base(bad) + ' cannot go inside itself.', 'bad'); return false; }
  const warn = risk(copying ? [dest] : work.concat(dest));
  if (warn && await ask(verb + ' ' + what(work) + ' to ' + folderName(dest) + '?', null, {warn, buttons: ['Cancel', verb]}) !== 1) return false;
  let there;
  try { there = await api('GET', '/api/list?hidden=1&path=' + enc(dest)); } catch (e) { fail(e); return false; }
  const here = new Map(there.entries.map(e => [e.name.toLowerCase(), e]));
  const clash = work.filter(p => parent(p) !== dest && here.has(base(p).toLowerCase()));
  let answer = '';
  if (clash.length) {
    const c = await askClash(clash, work.length, dest, copying, here, p => known.get(p) || entryOf(p));
    if (c < 1) return false;
    answer = ['', 'skip', 'keep', 'replace'][c];
  }
  const todo = work.filter(p => answer !== 'skip' || !clash.includes(p));
  if (!todo.length) { toast('Nothing to ' + verb.toLowerCase() + '.'); return false; }
  const landed = [], problems = [];
  for (const [i, p] of todo.entries()) {
    W.text = (copying ? 'Copying ' : 'Moving ') + (todo.length > 1 ? (i + 1) + ' of ' + todo.length : base(p));
    showStatus();
    const how = clash.includes(p) ? answer : parent(p) === dest ? 'keep' : '';
    try {
      const j = await api('POST', '/api/' + verb.toLowerCase(), {from: p, to: join(dest, base(p)), clash: how}, 0, {timeout: copying ? 4 * 3600e3 : 120e3});
      landed.push(j.path);
    } catch (e) {
      if (e.quiet) break;
      problems.push(base(p) + ': ' + e.message);
      if (S.offline) break;
    }
  }
  W.text = '';
  showStatus();
  if (landed.length) toast((copying ? 'Copied ' : 'Moved ') + what(landed) + ' to ' + folderName(dest), 'good');
  if (problems.length) ask('Not everything was ' + (copying ? 'copied' : 'moved'), problems.join('\n'), {buttons: ['OK']});
  if (dest === F.cwd) { for (const p of landed) F.fresh.add(p); F.sel = new Set(landed); }
  else if (!copying) for (const p of todo) { const node = F.nodes.get(p); if (node && landed.length) node.classList.add('gone'); }
  setTimeout(() => load(F.cwd, true), dest === F.cwd ? 0 : 280);
  return !problems.length;
}
// "2 of 3 items are already in saves": each with what is there and what
// is coming, the newer one marked.
function askClash(clash, total, dest, copying, here, source) {
  const box = document.createElement('div');
  box.className = 'clash';
  const line = (label, e, newer) => '<div><i>' + label + '</i><span>' + esc([e.dir ? 'Folder' : size(e.size), when(e.time)].filter(Boolean).join(', ')) + '</span>' + (newer ? '<span class="newer">Newer</span>' : '') + '</div>';
  for (const p of clash.slice(0, 4)) {
    const old = here.get(base(p).toLowerCase()), mine = source(p) || {name: base(p), dir: old.dir, size: -1, time: 0};
    const row = document.createElement('div');
    row.innerHTML = glyph(mine) + '<b>' + esc(base(p)) + '</b>' + line('Here', old, old.time > mine.time && mine.time > 0) + line(copying ? 'Copying' : 'Moving', mine, mine.time > old.time && old.time > 0);
    box.append(row);
  }
  if (clash.length > 4) box.insertAdjacentHTML('beforeend', '<span class="more">And ' + (clash.length - 4) + ' more</span>');
  const title = total === 1 ? base(clash[0]) + ' is already in ' + folderName(dest) : clash.length + ' of ' + total + ' items are already in ' + folderName(dest);
  return ask(title, null, {body: box, buttons: ['Cancel', 'Skip', 'Keep Both', 'Replace'], danger: true, left: true, warn: ''});
}

// --- Dragging -----------------------------------------------------------------------
// Items drag onto folders, the sidebar's places and the path at the bottom:
// a move, or a copy with Option (Ctrl elsewhere). Files from the computer
// dropped anywhere on the folder upload into it, or into a place dropped on.
const DRAG = 'application/x-3dsdl';
let dragging = [], dropOn = null, dropTimer = 0;
const copyKey = e => e.altKey || (!mac && e.ctrlKey);
items.addEventListener('dragstart', e => {
  const node = e.target.closest('.item');
  if (!node) return;
  if (!F.sel.has(node.dataset.path)) selectOnly(node.dataset.path);
  dragging = [...F.sel];
  e.dataTransfer.setData(DRAG, '1');
  e.dataTransfer.setData('text/plain', dragging.join('\n'));
  e.dataTransfer.effectAllowed = 'copyMove';
  const card = $('#dragcard');
  card.innerHTML = glyph(node.entry) + esc(what(dragging));
  e.dataTransfer.setDragImage(card, 18, 18);
});
items.addEventListener('dragend', () => { dragging = []; hover(null); });
function canTake(dest, copying) { return dragging.length && !dragging.some(p => inside(dest, p)) && (copying || !dragging.every(p => parent(p) === dest)); }
function hover(el, upload) {
  if (dropOn !== el) { if (dropOn) dropOn.classList.remove('drop'); dropOn = el; if (el && el.id !== 'pane') el.classList.add('drop'); }
  $('#pane').classList.toggle('dropping', !!upload);
  if (upload) $('#dropzone b').textContent = 'Drop to upload to ' + folderName(upload);
}
const canUpload = () => browsing() && !document.querySelector('dialog[open]');
addEventListener('dragover', e => {
  const types = [...e.dataTransfer.types], ours = types.includes(DRAG), files = types.includes('Files');
  if (!ours && !files) return;
  const target = e.target.closest && e.target.closest('[data-drop]'), pane = e.target.closest && e.target.closest('#pane');
  clearTimeout(dropTimer);
  dropTimer = setTimeout(() => hover(null), 160);
  if (ours) {
    if (!target || !canTake(target.dataset.drop, copyKey(e))) return hover(null);
    e.preventDefault();
    e.dataTransfer.dropEffect = copyKey(e) ? 'copy' : 'move';
    return hover(target);
  }
  e.preventDefault();
  const sideTarget = target && !pane ? target : null;
  if (!canUpload() || (!sideTarget && !pane)) { e.dataTransfer.dropEffect = 'none'; return hover(null); }
  e.dataTransfer.dropEffect = 'copy';
  hover(sideTarget || $('#pane'), sideTarget ? null : F.cwd);
});
addEventListener('drop', async e => {
  const types = [...e.dataTransfer.types], target = dropOn;
  hover(null);
  if (types.includes('Files')) e.preventDefault();
  if (!target) return;
  e.preventDefault();
  if (types.includes(DRAG)) { const paths = dragging; dragging = []; return transfer(paths, target.dataset.drop, copyKey(e)); }
  // Each dropped item keeps its folders when the browser can walk it; one it
  // cannot walk goes up on its own, without taking the others' folders away.
  // The items are only readable until the first await.
  const into = target.id === 'pane' ? F.cwd : target.dataset.drop, out = [];
  const dropped = [...e.dataTransfer.items].filter(i => i.kind === 'file')
    .map(i => ({entry: i.webkitGetAsEntry && i.webkitGetAsEntry(), file: i.getAsFile()}));
  if (!dropped.some(d => d.entry)) return pick(e.dataTransfer.files, into);
  for (const d of dropped) {
    if (d.entry) await walk(d.entry, into, out);
    else if (d.file) out.push({file: d.file, path: join(into, d.file.name)});
  }
  plan(out, into);
});

// --- Pictures and text ---------------------------------------------------------------
// The picture viewer, with the folder's other pictures a swipe or key away.
const V = {list: [], at: 0};
function view(path) {
  V.list = shown().filter(e => !e.dir && PICTURES.has(ext(e.name))).map(e => join(F.cwd, e.name));
  V.at = Math.max(0, V.list.indexOf(path));
  showPicture();
  $('#viewer').showModal();
}
function showPicture() {
  const path = V.list[V.at], img = $('#vimg');
  img.classList.add('loading');
  $('#vspin').hidden = false;
  img.onload = img.onerror = () => { img.classList.remove('loading'); $('#vspin').hidden = true; };
  img.src = fileUrl(path);
  $('#vname').textContent = base(path) + (V.list.length > 1 ? ', ' + (V.at + 1) + ' of ' + V.list.length : '');
  $('#vdl').href = fileUrl(path, true);
  $('#vprev').hidden = $('#vnext').hidden = V.list.length < 2;
}
const flip = n => { V.at = (V.at + n + V.list.length) % V.list.length; showPicture(); };
$('#vprev').onclick = () => flip(-1);
$('#vnext').onclick = () => flip(1);
$('#vclose').onclick = () => $('#viewer').close();
$('#viewer').addEventListener('keydown', e => { if (e.key === 'ArrowLeft') flip(-1); if (e.key === 'ArrowRight') flip(1); });
let touchX = null;
$('#viewer').addEventListener('touchstart', e => { touchX = e.touches[0].clientX; }, {passive: true});
$('#viewer').addEventListener('touchend', e => {
  if (touchX === null) return;
  const dx = e.changedTouches[0].clientX - touchX;
  if (Math.abs(dx) > 50 && V.list.length > 1) flip(dx < 0 ? 1 : -1);
  touchX = null;
});
// The text editor: CodeMirror from the 3DS (/editor.js), or the plain text
// box when it cannot load.
const E = {path: '', saved: '', code: null, loader: null};
function loadEditor() {
  if (!E.loader) E.loader = new Promise(resolve => {
    const s = document.createElement('script');
    s.src = '/editor.js';
    s.onload = () => resolve(window.CodeEditor || null);
    s.onerror = () => { E.loader = null; s.remove(); resolve(null); };
    document.head.append(s);
  });
  return E.loader;
}
async function edit(path) {
  const loading = loadEditor();
  let text;
  try {
    const r = await run(() => call('GET', fileUrl(path), undefined, {raw: true}));
    if (!r.ok) throw new Error('Could not open ' + base(path) + '.');
    text = await r.text();
  } catch (e) { return fail(e); }
  // A console busy with an upload may take a while; the text box does then.
  const code = await Promise.race([loading, sleep(3000).then(() => null)]);
  Object.assign(E, {path, saved: text});
  $('#ename').textContent = base(path);
  if (E.code) { E.code.destroy(); E.code = null; }
  if (code) {
    try { E.code = code.create({parent: $('#ecode'), text, name: base(path), change: editInfo, save}); } catch (e) {}
  }
  $('#ecode').hidden = !E.code;
  $('#etext').hidden = !!E.code;
  $('#etext').value = E.code ? '' : text;
  $('#eerr').hidden = true;
  editInfo();
  $('#editor').showModal();
  if (E.code) E.code.focus();
  else { $('#etext').focus(); $('#etext').setSelectionRange(0, 0); }
}
const editorText = () => E.code ? E.code.text() : $('#etext').value;
function editInfo() {
  const t = editorText();
  $('#editor').classList.toggle('changed', t !== E.saved);
  $('#einfo').textContent = plural(E.code ? E.code.lines() : t.split('\n').length, 'line') + ', ' + size(new Blob([t]).size) + (t !== E.saved ? ', not saved' : '');
}
$('#etext').addEventListener('input', editInfo);
async function save() {
  const b = $('#esave'), text = editorText();
  b.classList.add('busy');
  try {
    await api('PUT', '/api/file?replace=1&path=' + enc(E.path), text, 0, {retry: true});
    E.saved = text;
    editInfo();
    toast('Saved ' + base(E.path), 'good');
    load(F.cwd, true);
  } catch (e) { $('#eerr').textContent = e.message || 'Not saved.'; $('#eerr').hidden = false; } finally { b.classList.remove('busy'); }
}
async function closeEditor() {
  if (editorText() !== E.saved && await ask('Close without saving?', 'The changes to ' + base(E.path) + ' will be lost.', {buttons: ['Keep Editing', 'Close'], danger: true}) !== 1) return;
  $('#editor').close();
}
$('#esave').onclick = save;
$('#ecancel').onclick = closeEditor;
$('#eclose').onclick = closeEditor;
$('#editor').addEventListener('cancel', e => { e.preventDefault(); closeEditor(); });
$('#etext').addEventListener('keydown', e => {
  if ((e.ctrlKey || e.metaKey) && e.key === 's') { e.preventDefault(); save(); }
  if (e.key === 'Tab' && !e.shiftKey) { e.preventDefault(); document.execCommand('insertText', false, '\t'); }
});

// --- Uploads -----------------------------------------------------------------------
const U = {items: [], current: null, pill: '', made: new Set(), arrived: [], sent: 0, ms: 0};
async function plan(files, into = F.cwd) {
  if (!files.length) return;
  // Folders made for an earlier batch may have been deleted since.
  U.made.clear();
  const dirs = new Map();
  for (const f of files) dirs.set(parent(f.path), null);
  let checked = 0;
  if (dirs.size > 3) toast('Checking ' + dirs.size + ' folders on the 3DS...');
  for (const d of dirs.keys()) {
    try {
      const j = await api('GET', '/api/list?times=0&path=' + enc(d));
      dirs.set(d, new Map(j.entries.map(e => [e.name, e])));
    } catch (e) {
      if (e.quiet) return;
      // A folder that is not there yet is made when its files go up.
      if (S.offline || !/does not exist/.test(e.message)) return ask('Cannot upload', 'Could not check ' + d + ' on the 3DS: ' + e.message, {buttons: ['OK']});
      dirs.set(d, new Map());
    }
    if (++checked % 20 === 0) toast('Checked ' + checked + ' of ' + dirs.size + ' folders...');
  }
  const there = f => dirs.get(parent(f.path)), card = f => there(f).get(base(f.path));
  const blocked = files.find(f => card(f) && card(f).dir);
  if (blocked) return ask('Cannot upload', 'A folder already has the name ' + blocked.path + '.', {buttons: ['OK']});
  const clash = files.filter(f => card(f));
  let skip = new Set();
  if (clash.length) {
    const names = clash.slice(0, 5).map(f => base(f.path)).join('\n') + (clash.length > 5 ? '\nAnd ' + (clash.length - 5) + ' more' : '');
    const stop = {done: false};
    const c = await ask(clash.length === 1 ? 'Replace this file?' : 'Replace ' + clash.length + ' files?',
      names,
      {buttons: ['Cancel', 'Skip', 'Replace All', 'Replace Changed'], left: true, warn: risk(clash.map(f => f.path)), action: () => changed(clash, there, stop)});
    stop.done = true;
    if (c === null || c < 1) return;
    if (c === 1) skip = new Set(clash);
    else if (c === 2) clash.forEach(f => { f.replace = true; });
    else for (const f of clash) if (c.has(f)) f.replace = true; else skip.add(f);
  }
  files = files.filter(f => !skip.has(f));
  if (!files.length) return toast(clash.length ? 'Nothing to send: the SD card already has all of them.' : 'Nothing to send.');
  const need = files.reduce((n, f) => n + f.file.size, 0);
  if (F.free >= 0 && need > F.free) return ask('Not enough space', 'These need ' + size(need) + ' and the SD card has ' + size(F.free) + ' free.', {buttons: ['OK']});
  const big = files.find(f => f.file.size > 4294967295);
  if (big) return ask('Too big for the SD card', base(big.path) + " is 4 GiB or more, which the SD card's FAT32 cannot hold.", {buttons: ['OK']});
  // The console lists what arrives: each file says which drop it is part
  // of, and a folder dropped whole goes there as one row.
  const id = Date.now().toString(36) + Math.random().toString(36).slice(2, 6), top = into === '/' ? '/' : into + '/', folders = new Map();
  for (const f of files) {
    const rel = f.path.startsWith(top) ? f.path.slice(top.length) : '';
    f.folder = rel.includes('/') ? top + rel.split('/')[0] : '';
    if (f.folder) folders.set(f.folder, (folders.get(f.folder) || 0) + 1);
  }
  const bytes = files.reduce((n, f) => n + f.file.size, 0);
  let before = 0;
  files.forEach((f, i) => {
    f.batch = [id, i + 1, files.length, before, bytes].join(' ') + (f.folder ? ' ' + folders.get(f.folder) + ' ' + enc(f.folder) : '');
    before += f.file.size;
  });
  for (const f of files) {
    const li = document.createElement('li');
    li.innerHTML = svg('upload') + '<div><b></b><span class="s">Waiting</span></div><div></div><div class="track"><div class="fill"></div></div>';
    li.querySelector('b').textContent = f.path;
    Object.assign(f, {li, state: 'wait', loaded: 0});
    $('#uplist').prepend(li);
    U.items.push(f);
    buttons(f);
    run(() => send(f), 1, 'upload');
  }
  progress();
}
// CRC-32, as zlib computes it on the console, read in pieces so large
// files never sit in memory whole.
const CRC = Uint32Array.from({length: 256}, (_, n) => { for (let k = 0; k < 8; k++) n = n & 1 ? 0xedb88320 ^ (n >>> 1) : n >>> 1; return n >>> 0; });
async function crcOf(file) {
  let c = 0xffffffff;
  const reader = file.stream().getReader();
  for (;;) {
    const {done, value} = await reader.read();
    if (done) break;
    for (let i = 0; i < value.length; i++) c = CRC[(c ^ value[i]) & 255] ^ (c >>> 8);
  }
  return ((c ^ 0xffffffff) >>> 0).toString(16).padStart(8, '0');
}
// Which of clash differ from the copies on the SD card: by size, or else by
// content, whatever their dates say. The console checksums its copies in
// batches while this computer does its own.
// Stops, quietly, once stop.done is set: the dialog it runs in was closed.
async function changed(clash, there, stop) {
  const out = new Set(), same = new Map();
  for (const f of clash) {
    if (f.file.size !== there(f).get(base(f.path)).size) out.add(f);
    else { const d = parent(f.path); if (!same.has(d)) same.set(d, []); same.get(d).push(f); }
  }
  const total = [...same.values()].reduce((n, l) => n + l.length, 0), t0 = performance.now();
  let done = 0;
  for (const [d, list] of same) {
    let left = list;
    while (left.length) {
      const part = left.slice(0, 50);
      const [j, mine] = await Promise.all([
        api('POST', '/api/hash', {path: d, names: part.map(f => base(f.path)).join('\n')}, 1, {timeout: 120000, retry: true}),
        Promise.all(part.map(f => crcOf(f.file)))]);
      if (stop.done) throw quiet();
      const answered = part.filter(f => base(f.path) in j.hashes);
      if (!answered.length) throw new Error('The 3DS did not check ' + base(part[0].path) + '.');
      answered.forEach(f => { const h = j.hashes[base(f.path)]; if (!h || h !== mine[part.indexOf(f)]) out.add(f); });
      left = left.slice(answered.length);
      done += answered.length;
      const eta = done > 50 ? duration((performance.now() - t0) / 1e3 / done * (total - done)) : '';
      $('#dp').textContent = 'Comparing with the SD card: ' + done + ' of ' + total + ' files' + (eta ? ', ' + eta + ' left.' : '...');
    }
  }
  return out;
}
function buttons(f) {
  const box = f.li.children[2];
  box.textContent = '';
  const b = document.createElement('button');
  b.className = 'btn small';
  if (f.state === 'wait' || f.state === 'up') {
    b.textContent = 'Cancel';
    b.onclick = () => { if (f.xhr) f.xhr.abort(); else { f.state = 'off'; status(f, 'Cancelled', 'err'); buttons(f); progress(); } };
  } else if (f.state === 'failed' || f.state === 'off') {
    b.innerHTML = svg('retry') + 'Retry';
    b.onclick = () => { f.state = 'wait'; f.loaded = 0; status(f, 'Waiting'); f.li.className = ''; f.li.querySelector('.fill').style.width = '0'; buttons(f); run(() => send(f), 1, 'upload'); progress(); };
  } else return;
  box.append(b);
}
function status(f, text, kind) { const s = f.li.querySelector('.s'); s.textContent = text; s.className = 's' + (kind ? ' ' + kind : ''); }
// Says how the uploads stand: in Transfers, on its sidebar badge, in the
// status pill, and in a small card beside the folder while any are going.
function progress() {
  const live = U.items.filter(f => f.state !== 'off' && f.state !== 'failed');
  const left = live.filter(f => f.state === 'wait' || f.state === 'up');
  $('#upnone').hidden = !!U.items.length;
  $('#upcancel').hidden = !left.length;
  $('#upclear').hidden = left.length === U.items.length;
  $('#tbadge').hidden = !left.length;
  $('#tbadge').textContent = left.length;
  const minis = $$('.tmini');
  if (!left.length) {
    U.current = null;
    U.sent = 0;
    U.ms = 0;
    for (const m of minis) m.hidden = true;
    showStatus();
    return;
  }
  const total = live.reduce((n, f) => n + f.file.size, 0), done = live.reduce((n, f) => n + (f.state === 'done' ? f.file.size : f.loaded), 0);
  const rate = U.ms > 1000 ? U.sent / (U.ms / 1000) : 0;
  const pct = total ? Math.floor(100 * done / total) : 0;
  U.pill = 'Sending to the 3DS, ' + pct + '%';
  const rows = left.slice(0, 3).map(f => {
    const now = f.state === 'up', share = f.file.size ? 100 * f.loaded / f.file.size : 0;
    return '<div class="row' + (now ? ' active' : '') + '"><div class="line"><b>' + esc(base(f.path)) + '</b><span>' + esc(size(f.file.size)) + '</span></div><div class="track"><div class="fill" style="width:' + share.toFixed(1) +
      '%"></div></div><div class="line"><span>' + (now && rate ? esc(size(rate)) + '/s' : now ? 'Starting' : 'Waiting') + '</span><span>' + (now && rate ? esc(duration((f.file.size - f.loaded) / rate)) + ' left' : '') + '</span></div></div>';
  }).join('');
  const html = '<header><h2><a href="#transfers">Transfers</a></h2><button class="btn small" type="button">Stop All</button></header>' + rows +
    (left.length > 3 ? '<span class="muted">And ' + (left.length - 3) + ' more</span>' : '');
  for (const m of minis) {
    m.hidden = !onFiles() || (m.parentNode.id === 'insp') !== wide.matches;
    m.innerHTML = html;
    m.querySelector('button').onclick = stopAll;
  }
  showStatus();
}
async function send(f) {
  if (f.state !== 'wait') return;
  f.state = 'up';
  U.current = f;
  f.li.classList.add('active');
  status(f, 'Starting...');
  buttons(f);
  progress();
  const dir = parent(f.path);
  if (dir !== F.cwd && !U.made.has(dir)) {
    try { await call('POST', '/api/folder', {path: dir}, {retry: true}); U.made.add(dir); } catch (e) { return finish(f, 'failed', e.message || 'Not allowed', 'err'); }
  }
  await new Promise(resolve => {
    const x = new XMLHttpRequest(), fill = f.li.querySelector('.fill');
    let last = performance.now(), lastBytes = 0;
    const t0 = last;
    f.xhr = x;
    x.open('PUT', '/api/file?path=' + enc(f.path) + (f.replace ? '&replace=1' : ''));
    x.setRequestHeader('X-Requested-With', '3dsDL');
    x.setRequestHeader('X-3dsDL-Batch', f.batch);
    x.upload.onprogress = e => {
      if (!e.lengthComputable) return;
      const now = performance.now();
      U.sent += e.loaded - lastBytes;
      U.ms += now - last;
      last = now;
      lastBytes = e.loaded;
      f.loaded = e.loaded;
      const s = (now - t0) / 1e3;
      fill.style.width = (e.total ? 100 * e.loaded / e.total : 100) + '%';
      status(f, size(e.loaded) + ' of ' + size(e.total) + (s > 1 ? ', ' + size(e.loaded / s) + '/s' : ''));
      progress();
    };
    const end = (state, text, kind) => { f.xhr = null; finish(f, state, text, kind); resolve(); };
    x.onload = () => {
      let j = {};
      try { j = JSON.parse(x.responseText); } catch (e) {}
      if (x.status === 200) {
        fill.style.width = '100%';
        const s = (performance.now() - t0) / 1e3;
        f.li.classList.add('ok');
        if (CIAS.has(ext(f.path))) U.arrived.push(f.path);
        const top = f.path.startsWith(F.cwd === '/' ? '/' : F.cwd + '/') ? f.path.slice(F.cwd === '/' ? 1 : F.cwd.length + 1).split('/')[0] : '';
        if (top && !F.entries.some(e => e.name === top)) {
          // Shown now, ahead of the next upload, rather than once all are
          // done. Something already listed waits for the end: each listing
          // keeps the console from the next upload.
          F.fresh.add(join(F.cwd, top));
          load(F.cwd, true, 0);
        }
        end('done', 'Done, ' + size(f.file.size) + (s > 1 ? ' at ' + size(f.file.size / s) + '/s' : ''), 'ok');
      } else {
        if (x.status === 403 && j.access) gate(j.access);
        end('failed', j.error || 'Failed (' + x.status + ')', 'err');
      }
    };
    x.onerror = () => {
      // Sent again a couple of times before it counts as failed. The first
      // try may have landed before the answer was lost, so the next one
      // replaces whatever it left.
      if ((f.tries = (f.tries || 0) + 1) <= 2) {
        f.xhr = null;
        f.state = 'wait';
        f.replace = true;
        f.loaded = 0;
        status(f, 'Trying again...');
        resolve();
        sleep(1500 * f.tries).then(() => run(() => send(f), 1, 'upload'));
        return;
      }
      offline(true);
      end('failed', 'The connection to the 3DS was lost.', 'err');
    };
    x.onabort = () => end('off', 'Cancelled', 'err');
    x.send(f.file);
  });
}
function finish(f, state, text, kind) {
  f.state = state;
  f.li.classList.remove('active');
  if (state !== 'done') f.li.classList.add('no');
  status(f, text, kind);
  buttons(f);
  if (U.current === f) U.current = null;
  progress();
  if (!U.items.some(i => i.state === 'wait' || i.state === 'up')) {
    load(F.cwd, true);
    if ($('#offer').checked && U.arrived.length) install(U.arrived);
    U.arrived = [];
  }
}
function stopAll() {
  for (const f of U.items) if (f.state === 'wait') { f.state = 'off'; status(f, 'Cancelled', 'err'); f.li.classList.add('no'); buttons(f); }
  if (U.current && U.current.xhr) U.current.xhr.abort();
  progress();
}
$('#upcancel').onclick = stopAll;
$('#upclear').onclick = () => {
  U.items = U.items.filter(f => { const keep = f.state === 'wait' || f.state === 'up'; if (!keep) f.li.remove(); return keep; });
  progress();
};
$('#offer').checked = store.get('offer', true);
$('#offer').onchange = () => store.set('offer', $('#offer').checked);
addEventListener('beforeunload', e => { if (U.items.some(f => f.state === 'wait' || f.state === 'up')) { e.preventDefault(); e.returnValue = ''; } });
const pick = (list, into = F.cwd) => plan([...list].map(file => ({file, path: join(into, file.webkitRelativePath || file.name)})), into);
$('#pick').onchange = e => { pick(e.target.files); e.target.value = ''; };
if ($('#pickdir')) $('#pickdir').onchange = e => { pick(e.target.files); e.target.value = ''; };
function walk(entry, dir, out) {
  return new Promise(resolve => {
    if (entry.isFile) entry.file(file => { out.push({file, path: join(dir, file.name)}); resolve(); }, () => resolve());
    else if (entry.isDirectory) {
      const reader = entry.createReader(), all = [];
      const more = () => reader.readEntries(async got => {
        if (got.length) { all.push(...got); more(); } else { for (const g of all) await walk(g, join(dir, entry.name), out); resolve(); }
      }, () => resolve());
      more();
    } else resolve();
  });
}

route();
poll();
</script></body></html>)PAGE";

} // namespace

std::string page(const std::string &message, bool good)
{
    std::string html = PageStart;
    if (!message.empty()) {
        html += std::string("<div class=\"note ") + (good ? "good" : "bad") +
                "\">" + escapeHtml(message) + "</div>\n";
    }
    return html + PageEnd;
}

} // namespace remote
