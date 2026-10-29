#include "remote.hpp"

// The page a phone or computer sees on the Receive screen's address: a box
// for links, which works without JavaScript, and a file manager for the SD
// card that talks to the JSON API in net/remote_server. Colours follow
// docs/DESIGN.md.
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
<meta name="theme-color" content="#f2ebb2">
<link rel="icon" type="image/png" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAIAAAD8GO2jAAAABGdBTUEAALGPC/xhBQAAACBjSFJNAAB6JgAAgIQAAPoAAACA6AAAdTAAAOpgAAA6mAAAF3CculE8AAAARGVYSWZNTQAqAAAACAABh2kABAAAAAEAAAAaAAAAAAADoAEAAwAAAAEAAQAAoAIABAAAAAEAAAAgoAMABAAAAAEAAAAgAAAAAKyGYvMAAAHLaVRYdFhNTDpjb20uYWRvYmUueG1wAAAAAAA8eDp4bXBtZXRhIHhtbG5zOng9ImFkb2JlOm5zOm1ldGEvIiB4OnhtcHRrPSJYTVAgQ29yZSA2LjAuMCI+CiAgIDxyZGY6UkRGIHhtbG5zOnJkZj0iaHR0cDovL3d3dy53My5vcmcvMTk5OS8wMi8yMi1yZGYtc3ludGF4LW5zIyI+CiAgICAgIDxyZGY6RGVzY3JpcHRpb24gcmRmOmFib3V0PSIiCiAgICAgICAgICAgIHhtbG5zOmV4aWY9Imh0dHA6Ly9ucy5hZG9iZS5jb20vZXhpZi8xLjAvIj4KICAgICAgICAgPGV4aWY6Q29sb3JTcGFjZT4xPC9leGlmOkNvbG9yU3BhY2U+CiAgICAgICAgIDxleGlmOlBpeGVsWERpbWVuc2lvbj4yNTY8L2V4aWY6UGl4ZWxYRGltZW5zaW9uPgogICAgICAgICA8ZXhpZjpQaXhlbFlEaW1lbnNpb24+MjU2PC9leGlmOlBpeGVsWURpbWVuc2lvbj4KICAgICAgPC9yZGY6RGVzY3JpcHRpb24+CiAgIDwvcmRmOlJERj4KPC94OnhtcG1ldGE+CuYattQAAAaYSURBVEgNtVZLj1zFFT6n7rsfM+72zBhkbAG2F4MdBRBIVpAiWHiRSNmBkggJWWILO9ggJK9gwY6fkLBIFLFBCkIIhIWExSPiYYVFMBo0YxsznnGPZ9zT3fdRD75T1T1j7Ci7lG5X1617zvedR51zL1vrmOn/N2JH5PDDAI1f/He62S5ERPA2yaAdTJxJhTuZQRBULJmB4wWl2DqBmA3/1G1w/SmZBvLkuOGH4uKEZwmCUwuh8gtVDxEDDvsRr3DzsWm6Y/dY0T0mm+xpAnv5T2W+ock1YsXU3Nxa7h9/TampDfvw0w35E1Q/FDiNmYw2P2bWur5cTrYFmCEARefYi1pLqiDO/FWQ2zWmgaIIzKBEQS6vddusLECilk2furI6WlltWyruEIEdjjrBGpkZXpRGV7IOkNN/3IFSLgQggGAhScZd1n2wzWcXOgcjRQYiPjw+wOIE85zAMYJi8ReBAPkQ5+S35wQ8U3bV0BJFS3uuKaygBJpW9yDEcWrB53X8DHS7TmpMNPJSmLjItar+xfoHZ2sh9kMMr1bi+gOz+5GWAIo6RiwmAR7YYqsf3n7ZY25uvZ9Wb8VJREhDGI6KIiL+6+D7oZ575eDh085KMoBQm/76tV6n90BL+dPv5X2IsJJICY1wePsjpoTJuqPXVpJD/c2s3yIVwXwv5oYbu4PJmSPHHgYvVGEtfmn7yOKxs0FfNkSUeKc2UBI9j44J65hp09InJY0sj4c3dq5/uNzfeah1JefGEt8o849+OvXsqT/c184qia/HBwIsk6QEPr8A3HZtgC0cUwKC7SNLb4z5i21T1U6pCOfGWcqpQaSg37i4pOhYs3PuvuxENy9NMDa44XE9WYgpjul0yydF1qil7yu7VtmWcZIeY5zWbHVlVaMyqzLsFaTX4vZLl6vPB+M8Ap5Yjf29Sxzz1/SYTmMkkiIaOTmssaI/9dTxDMlgBI2tvbR21Rnz6PHDeRwhQ2M79/aPt7aa4ZlD3Qp+BI49Jr+IsSvomDH8AsscDcG6XxXqxcVZQyCajOq/vXd+PKmefvGZfmfeK9BksfXy2jiy20/ec6BG0e5zCDDOloIv+1HyRNjJlJzdZB9c0KIsTbOsNG5YoetNRy9yNs0v3NiFbXfioMKcm+YAz3AjFjiH4CSAIxp7c2ZQdP6r724OUXH0l/c/2x2XYb8dca5415KWEhWOGZTPgSNUMnClmENawgLxjZwbo4WH0HmwfrdAYZR1c3TxQJGngaCluFB0y1JjhCBcASTAhkp2PhiMB6HdoFITchM5kVITYTy+/MCrz+Vff7f2xzOnp1toMIo7QsCVMTHDXAEJmQAB+OR9IFUgpS5tTTQZrweXkivFA8jI5rXBzqXL17Wx3U777x98MS6r350+ee9iP1cuJhqSKrVtx0AXBeGYLni/FwE9mA8iLHDaNxytTsyptoLwD1fWL1y8lOVplCYcxypN/zNq1nj34sRdt62E1agxrRwJkL7kbWdQgek2D6TfSZVhhlEZE9rAuXVzIiprR9X80frJ+yviynFNjGb9TmP0FllWieKa1bDRC2I7cuo9mM0x0gh74Rfs9KGSW0Ql87cDF63rGKwYsAvJ1wAgLFxNCpXoS9IZFQ0bhBOvmmB+mL3FULvjAiVAMh9KIOOkI+FddsuZQxIfKfg3BSGxLyyo5cT+fl69fg8j1Tdr0QMJOLAKmFhIDgCHPMvsTxFm6z1AZH5d8CuH1FsbVS9Pz/bp3JX6+aV8gfSbm/bPvXjOuiylJzp8OFUDf04F1wOGGc4iB+KypMafIdyAAMzwQDs6ktC9CZ9M3S12CUdLkZ2greLkorvKGaUbOMjEnVhtTyzMFy9mHIILi2E3LnR5tE4znT0BeEgqCCNXNISq71Ej4zI0dF+3vYhGWrr9fMRbGgSiqL192IUqrvBdJCgYUz8gpwh1AIqJx51jt4M3DXasXSv1ydj+qPnCTnO5tOcnSXuzvFirk9YYX/mIUrBdAEFw2608wGOECsHJPd+KVu8Oqq9H9KmJ9dXy8zIZjd0/Nuqrmi9uj61u8Kr4tqmXdPnb44cqmL+H7S3GHX9zc781+k0hRgy2G/3mT6PV2hqtI92kpsGH2RyZDlkcoYOJOpDEvTQ5kCVzWbLUabfzXMM6MfoXg7+6iwAiEIyZt8v63xsDsqaXJfNZ2k2TIk2yGN8Y+IJFQ0dE4bEkFo3Ov/zvggfhl1t3ezA1QTGjSiEjR0NC6Q8clrOwguB/D5z+nwFBOSw475JwzgAAAABJRU5ErkJggg=="><title>3dsDL</title><style>
:root{color-scheme:light;--ink:#615a4b;--soft:#8c8466;--text:#141414;--blue:#1c93c2;--panel:#dcd49a;--recess:#d9d3a0;--dots:#b0a66e;--good:#3fa65a;--bad:#e0301a;--warn:#e69614;--cream:#fffdf4;--ease:cubic-bezier(.2,.8,.2,1);--spring:cubic-bezier(.2,.9,.3,1.25)}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html{background:#f0e694}
body{margin:0;font:16px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif;background:linear-gradient(#f2ebb2,#f0e694);color:var(--ink);min-height:100vh}
main{max-width:48rem;margin:0 auto;padding:16px 16px 140px}
[hidden]{display:none!important}
.i{width:18px;height:18px;flex:none;fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.i.fill{fill:currentColor;stroke:none}
.head{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:12px}
h1{margin:0;font-size:1.55rem;color:var(--blue);letter-spacing:-.01em;line-height:1.1}
.brand{display:flex;flex-direction:column;gap:3px;min-width:0}
.logo{display:flex;align-items:center;gap:9px;text-decoration:none}
.logo img{width:30px;height:30px;border-radius:8px;box-shadow:0 1px 3px #0002;transition:transform .3s var(--spring)}
.logo:hover img{transform:rotate(-8deg) scale(1.08)}
.ext{display:flex;gap:12px;padding-left:39px;font-size:12px;font-weight:600}
.ext a{display:inline-flex;align-items:center;gap:3px;color:var(--soft);text-decoration:none;transition:color .2s}
.ext a:hover{color:var(--blue)}
.ext .i{width:11px;height:11px}
.pill{display:inline-flex;align-items:center;gap:8px;min-width:0;max-width:62%;padding:6px 12px 6px 10px;border-radius:999px;background:#fffdf0cc;border:1px solid #d6cc8f;font-size:13px;font-weight:600;color:var(--ink);box-shadow:0 1px 2px #0001;transition:background .3s,border-color .3s}
.pill span{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.dot{width:9px;height:9px;border-radius:50%;flex:none;background:var(--good)}
.pill[data-state=connecting] .dot{background:#aca27a;animation:pulse 1.2s infinite}
.pill[data-state=asking] .dot,.pill[data-state=choosing] .dot{background:var(--warn);color:#e6961466;animation:ring 1.4s infinite}
.pill[data-state=asking],.pill[data-state=choosing]{background:#fff3cf;border-color:#ebc56a}
.pill[data-state=installing] .dot,.pill[data-state=downloading] .dot,.pill[data-state=sending] .dot,.pill[data-state=busy] .dot{background:none;border:2px solid #9fd3ea;border-top-color:var(--blue);animation:spin .8s linear infinite}
.pill[data-state=offline]{background:#ffe5df;border-color:#f0a596;color:#9a1d0b}
.pill[data-state=offline] .dot{background:var(--bad)}
@keyframes ring{0%{box-shadow:0 0 0 0 currentColor}70%,100%{box-shadow:0 0 0 7px transparent}}
@keyframes pulse{50%{opacity:.35}}
@keyframes spin{to{transform:rotate(1turn)}}
.tabs{position:relative;display:flex;background:var(--recess);border-radius:12px;padding:3px;box-shadow:inset 0 1px 2px #0002;margin-bottom:16px}
.tabs a{position:relative;z-index:1;flex:1;text-align:center;padding:8px 16px;border-radius:9px;color:var(--ink);text-decoration:none;font-weight:700;transition:color .25s}
.tabs a.on{color:var(--blue)}
.tabs .ink{position:absolute;top:3px;bottom:3px;left:3px;width:0;border-radius:9px;background:linear-gradient(#fefcf5,#e8e3cf);box-shadow:0 1px 3px #0003;transition:left .3s var(--ease),width .3s var(--ease)}
p{margin:0 0 14px}
.muted{color:var(--soft);font-size:14px}
.panel{background:var(--panel);border-radius:14px;padding:14px}
textarea,input[type=text],input[type=search]{width:100%;border:1px solid #b3aa83;border-radius:10px;padding:10px 12px;font:15px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace;background:var(--cream);color:var(--text);transition:border-color .2s,box-shadow .2s}
textarea:focus,input[type=text]:focus,input[type=search]:focus{outline:none;border-color:var(--blue);box-shadow:0 0 0 3px #1c93c233}
textarea{min-height:9rem;resize:vertical}
.btn{display:inline-flex;align-items:center;justify-content:center;gap:7px;padding:9px 14px;border:1px solid #8f8659;border-radius:11px;font:600 15px system-ui,-apple-system,sans-serif;color:#756e60;background:linear-gradient(#fefcf5,#e0dccb);cursor:pointer;white-space:nowrap;position:relative;text-decoration:none;transition:transform .12s var(--ease),box-shadow .2s,background .2s;box-shadow:0 1px 0 #fff8 inset,0 1px 2px #0001}
.btn:hover{background:linear-gradient(#fffef9,#e8e4d4)}
.btn:active{transform:scale(.97);background:#e0dccb}
.btn.primary{color:#fff;border-color:#10668a;background:linear-gradient(#35aee0,#1683b0);box-shadow:0 1px 0 #fff5 inset,0 2px 6px #1c93c244}
.btn.primary:hover{background:linear-gradient(#45b8e8,#1a8cbc)}
.btn.danger{color:#fff;border-color:#9a1d0b;background:linear-gradient(#ef5a3c,#c8260f)}
.btn.small{padding:5px 10px;font-size:13px;border-radius:9px}
.btn.icon-only{padding:9px 10px}
.btn:disabled{opacity:.45;cursor:default;transform:none}
.btn.busy{color:transparent!important;pointer-events:none}
.btn.busy .i{opacity:0}
.btn.busy::after{content:"";position:absolute;inset:0;margin:auto;width:17px;height:17px;border-radius:50%;border:2.5px solid #8c846655;border-top-color:#756e60;animation:spin .7s linear infinite}
.btn.primary.busy::after,.btn.danger.busy::after{border-color:#fff6;border-top-color:#fff}
button:focus-visible,.btn:focus-within,a:focus-visible,.row:focus-visible{outline:3px solid #3cdc78;outline-offset:2px}
.spin{display:inline-block;flex:none;width:18px;height:18px;border-radius:50%;border:2.5px solid #cfc795;border-top-color:var(--blue);animation:spin .8s linear infinite;vertical-align:-4px}
.note{border-radius:12px;padding:10px 12px;margin-bottom:14px;color:#fff;animation:rise .35s var(--ease)}
.good{background:var(--good)}.bad{background:var(--bad)}
small{display:block;margin-top:14px;color:var(--soft)}
.check{display:flex;gap:8px;align-items:center;font-size:14px;cursor:pointer}
input[type=checkbox]{width:18px;height:18px;accent-color:var(--blue);flex:none;cursor:pointer}
@keyframes rise{from{opacity:0;transform:translateY(8px)}}
.formrow{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-top:10px}
.formrow .btn{padding:11px 22px;font-size:16px}
.count{font-size:14px;color:var(--soft);transition:color .2s}
.count.has{color:var(--blue);font-weight:600}
.shake{animation:shake .4s}
@keyframes shake{20%,60%{transform:translateX(-6px)}40%,80%{transform:translateX(6px)}}
.sent{list-style:none;margin:14px 0 0;padding:0;display:grid;gap:8px}
.sent li{display:flex;gap:10px;align-items:flex-start;background:#fffdf0b0;border:1px solid #e3d99b;border-radius:12px;padding:10px 12px;animation:rise .35s var(--ease)}
.sent li .i{margin-top:2px;color:var(--blue)}
.sent li.ok .i{color:var(--good)}.sent li.no .i{color:var(--bad)}
.sent b{display:block;color:var(--text);font-size:15px;overflow-wrap:anywhere}
.sent span{font-size:13px;color:var(--soft)}
.banner{display:flex;gap:10px;align-items:center;background:#ffe5df;border:1px solid #f0a596;color:#9a1d0b;border-radius:12px;padding:10px 12px;margin-bottom:14px;font-size:14px;animation:rise .3s var(--ease)}
.banner .spin{border-color:#f0a596;border-top-color:#9a1d0b}
.card{display:flex;gap:12px;align-items:center;background:linear-gradient(#fff8dc,#fff0b8);border:1px solid #ebc56a;border-radius:14px;padding:12px 14px;margin-bottom:12px;animation:rise .35s var(--ease)}
.card .art{width:40px;height:40px;border-radius:50%;display:grid;place-items:center;background:#fff;color:var(--warn);flex:none;box-shadow:0 1px 3px #0002}
.card b{display:block;color:var(--text)}
.card span{font-size:13px;color:var(--ink)}
.card .art .i{width:22px;height:22px}
.card[data-state=choosing] .art,.card[data-state=asking] .art{animation:bob 1.6s ease-in-out infinite}
@keyframes bob{50%{transform:translateY(-3px)}}
.storage{display:flex;align-items:center;gap:10px;margin-bottom:10px;font-size:13px;color:var(--soft)}
.meter{flex:1;height:7px;border-radius:4px;background:#cfc795;overflow:hidden;box-shadow:inset 0 1px 2px #0002}
.meter i{display:block;height:100%;width:0;border-radius:4px;background:linear-gradient(90deg,#35aee0,#1683b0);transition:width .6s var(--ease)}
.meter.full i{background:linear-gradient(90deg,#ffb020,#e0301a)}
.pathbar{display:flex;align-items:center;gap:8px;margin-bottom:10px;min-width:0}
.crumbs{display:flex;flex-wrap:wrap;align-items:center;gap:2px;font-weight:700;min-width:0}
.crumbs a{color:var(--blue);text-decoration:none;padding:3px 6px;border-radius:7px;overflow-wrap:anywhere;transition:background .2s}
.crumbs a:hover{background:#fff8}
.crumbs a:last-child{color:var(--text)}
.crumbs span{color:var(--dots)}
.tools{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:8px}
.tools2{display:flex;flex-wrap:wrap;align-items:center;gap:8px;margin-bottom:10px}
.search{position:relative;flex:1 1 200px}
.search .i{position:absolute;left:11px;top:50%;transform:translateY(-50%);color:var(--soft);pointer-events:none}
.search input{padding-left:36px;font-family:inherit}
select{padding:9px 10px;border-radius:11px;border:1px solid #8f8659;background:var(--cream);color:var(--ink);font:inherit;font-size:15px}
.reload.busy{color:#756e60!important;pointer-events:auto}
.reload.busy::after{display:none}
.reload.busy .i{opacity:1;animation:spin .8s linear infinite}
.listwrap{position:relative;background:var(--recess);border-radius:14px;box-shadow:inset 0 1px 3px #0002;overflow:hidden}
.loadline{position:absolute;left:0;right:0;top:0;height:3px;overflow:hidden;opacity:0;transition:opacity .2s}
.loadline.on{opacity:1}
.loadline::after{content:"";position:absolute;top:0;bottom:0;width:40%;background:var(--blue);border-radius:2px;animation:slide 1s var(--ease) infinite}
@keyframes slide{from{left:-40%}to{left:100%}}
.listhead{display:flex;align-items:center;gap:10px;padding:9px 14px 4px;font-size:13px;color:var(--soft)}
.list{list-style:none;margin:0;padding:0 8px 6px;transition:opacity .2s}
.list.dim{opacity:.55}
.row{display:flex;align-items:center;gap:10px;padding:8px 6px;border-radius:10px;cursor:pointer;position:relative;transition:background .15s}
.row::after{content:"";position:absolute;left:8px;right:8px;bottom:0;height:3px;background-image:radial-gradient(circle,var(--dots) 1.2px,transparent 1.5px);background-size:8px 3px;background-repeat:repeat-x}
.row:last-child::after{display:none}
.row:hover{background:#ffffff50}
.row.on{background:#fff0b4}
.row.anim{animation:rise .3s var(--ease) both;animation-delay:calc(var(--i,0)*16ms)}
.row.fresh{animation:fresh 1.8s ease}
@keyframes fresh{0%,30%{background:#bfe8c8}100%{background:transparent}}
.row.gone{animation:gone .28s var(--ease) forwards;pointer-events:none}
@keyframes gone{to{opacity:0;transform:translateX(18px)}}
.ic{flex:none;width:42px;height:42px;border-radius:10px;display:grid;place-items:center;font:800 10px system-ui,sans-serif;color:#fff;letter-spacing:.02em;box-shadow:inset 0 -2px 0 #0002;overflow:hidden}
.ic svg{width:28px;height:28px}
.ic img{width:100%;height:100%;animation:fadein .3s ease}
@keyframes fadein{from{opacity:0}}
.ic.img{background:#fff;box-shadow:0 1px 3px #0003}
.k-dir{background:none;box-shadow:none}.k-cia{background:#1c93c2}.k-3dsx{background:#3fa65a}.k-zip{background:#b07a2a}.k-img{background:#c0569a}.k-txt{background:#8c8466}.k-rom{background:#e69614}.k-file{background:#aca27a}.k-media{background:#7a5cc0}
.what{flex:1;min-width:0}
.name{display:block;color:var(--text);font-weight:600;overflow-wrap:anywhere}
.name mark{background:#ffe066;color:inherit;border-radius:3px;padding:0 1px}
.meta{font-size:13px;color:var(--soft)}
.more{flex:none;display:grid;place-items:center;width:36px;height:36px;border-radius:10px;border:1px solid transparent;background:none;color:var(--ink);cursor:pointer;transition:background .15s,border-color .15s}
.more:hover{background:#fffdf0;border-color:#d6cc8f}
.empty{text-align:center;color:var(--soft);padding:34px 12px;display:grid;justify-items:center;gap:2px}
.empty .i{width:40px;height:40px;color:#c4b97c;margin-bottom:6px}
.empty b{color:var(--ink)}
.skel{display:flex;align-items:center;gap:10px;padding:8px 6px}
.skel i,.skel b{display:block;border-radius:9px;background:linear-gradient(90deg,#cfc792 25%,#e4dcaa 50%,#cfc792 75%);background-size:200% 100%;animation:shimmer 1.2s linear infinite}
.skel i{width:42px;height:42px}
.skel b{height:12px;flex:1;max-width:var(--w,60%)}
@keyframes shimmer{from{background-position:200% 0}to{background-position:-200% 0}}
.ups{position:sticky;top:8px;z-index:5;margin-bottom:12px;padding:12px 14px;animation:rise .3s var(--ease);transition:box-shadow .25s}
.ups.stuck{box-shadow:0 8px 24px #3a320f33}
.uphead{display:flex;flex-wrap:wrap;align-items:center;gap:8px 10px}
.uphead>div:first-child{flex:1 1 220px;min-width:0}
.uphead b{display:block;color:var(--text);font-size:15px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.uphead span{display:block;font-size:13px;color:var(--soft);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.uphead .track{order:3;flex:1 1 100%;height:8px}
.upbtns{order:4;display:flex;flex-wrap:wrap;gap:6px}
.ups.done .uphead .fill{animation:none;background:linear-gradient(#7fd08f,#3fa65a)}
.ups ul{list-style:none;margin:8px 0 0;padding:0 4px 0 0;max-height:min(34vh,240px);overflow-y:auto;overscroll-behavior:contain}
.ups:not(.open) .updetail{display:none}
.toggle .i{transition:transform .25s var(--ease)}
.ups.open .toggle .i{transform:rotate(180deg)}
.ups li{display:grid;grid-template-columns:1fr auto;gap:3px 10px;align-items:center;padding:9px 0;border-bottom:1px dotted var(--dots);animation:rise .3s var(--ease)}
.ups li:last-child{border:0}
.ups .n{font-weight:600;overflow-wrap:anywhere;min-width:0;color:var(--text);font-size:14px}
.ups .s{font-size:13px;color:var(--soft);grid-column:1}
.ups .s.err{color:var(--bad)}.ups .s.ok{color:var(--good)}
.ups li>div:last-child{grid-row:1/span 3;grid-column:2;display:flex;gap:6px}
.track{grid-column:1;height:10px;border-radius:5px;background:#cfc795;overflow:hidden;box-shadow:inset 0 1px 2px #0002}
.fill{height:100%;width:0;border-radius:5px;background:linear-gradient(#ffcc22,#ff9900);transition:width .25s linear}
.active .fill,.uphead .fill{background-image:linear-gradient(45deg,#fff4 25%,transparent 25%,transparent 50%,#fff4 50%,#fff4 75%,transparent 75%),linear-gradient(#ffcc22,#ff9900);background-size:18px 18px,100% 100%;animation:stripes .8s linear infinite}
.ok .fill{background:linear-gradient(#7fd08f,#3fa65a)}
@keyframes stripes{to{background-position:18px 0,0 0}}
.dock{position:fixed;left:0;right:0;bottom:0;z-index:15;display:grid;gap:8px;padding:0 12px calc(12px + env(safe-area-inset-bottom));pointer-events:none}
.dock>*{pointer-events:auto;max-width:46rem;width:100%;margin:0 auto;animation:dockin .35s var(--spring)}
@keyframes dockin{from{opacity:0;transform:translateY(24px)}}
.selbar{display:flex;flex-wrap:wrap;align-items:center;gap:8px;background:#fffdf0f2;backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px);border:1px solid #d6cc8f;border-radius:16px;padding:10px 12px;box-shadow:0 8px 28px #3a320f30}
.selbar b{margin-right:auto;color:var(--text)}
.drop{position:fixed;inset:10px;z-index:30;display:grid;place-items:center;background:rgba(28,147,194,.16);border:3px dashed var(--blue);border-radius:22px;color:var(--blue);pointer-events:none;animation:fadein .15s}
.drop div{display:grid;justify-items:center;gap:6px;background:#fffdf0;padding:22px 30px;border-radius:18px;box-shadow:0 10px 30px #0002;animation:pop .3s var(--spring)}
.drop .i{width:36px;height:36px}
.drop b{font-size:1.2rem}.drop span{color:var(--ink);font-size:14px}
.toasts{position:fixed;left:16px;right:16px;top:14px;z-index:40;display:flex;flex-direction:column;align-items:center;gap:8px;pointer-events:none}
.toast{display:flex;gap:9px;align-items:center;max-width:30rem;background:#1e1c16f0;color:#fff;padding:10px 16px;border-radius:12px;font-size:15px;box-shadow:0 8px 24px #0003;animation:toastin .35s var(--spring);transition:opacity .3s,transform .3s;overflow-wrap:anywhere}
.toast.good .i{color:#7fe08f}.toast.bad .i{color:#ff8a73}
.toast.out{opacity:0;transform:translateY(-10px) scale(.97)}
@keyframes toastin{from{opacity:0;transform:translateY(-14px) scale(.96)}}
.gate{text-align:center;padding:30px 18px;animation:rise .3s var(--ease)}
.gate h2{margin:4px 0 6px;font-size:1.2rem;color:var(--text)}
.gate .art{width:64px;height:64px;margin:0 auto 6px;border-radius:50%;display:grid;place-items:center;background:#fffdf0;color:var(--warn);box-shadow:0 2px 8px #0002;animation:bob 1.8s ease-in-out infinite}
.gate .art .i{width:30px;height:30px}
.gate.no .art{color:var(--bad);animation:none}
.waiting{display:inline-flex;gap:8px;align-items:center;margin-top:6px;padding:7px 14px;border-radius:999px;background:#fffdf0;font-size:14px;font-weight:600}
dialog{border:0;border-radius:18px;padding:0;width:min(30rem,calc(100vw - 24px));max-height:calc(100vh - 24px);background:linear-gradient(#fffdf0,#ebe9e1);color:var(--text);box-shadow:0 18px 50px #0006}
dialog[open]{animation:pop .28s var(--spring)}
dialog::backdrop{background:rgba(20,18,8,.48);animation:fadein .2s}
@keyframes pop{from{opacity:0;transform:translateY(10px) scale(.95)}}
.dbody{padding:20px 20px 4px}
dialog h2{margin:0 0 8px;font-size:1.15rem}
dialog p{white-space:pre-line;overflow-wrap:anywhere;color:var(--ink)}
.warnbox{display:flex;gap:8px;align-items:flex-start;background:#fff0d6;border:1px solid #f0c27a;color:#8a4b00;border-radius:10px;padding:9px 11px;margin:0 0 12px;font-size:14px}
.err{color:var(--bad);font-size:14px;margin:8px 0 0}
.row2{display:flex;justify-content:flex-end;flex-wrap:wrap;gap:8px;padding:12px 20px 18px}
.sheet .top{display:flex;gap:14px;align-items:center;padding:20px 54px 10px 20px}
.sheet .ic{width:64px;height:64px;border-radius:14px;font-size:13px}
.sheet .ic svg{width:40px;height:40px}
.sheet h2{margin:0;font-size:1.1rem;overflow-wrap:anywhere}
.sheet .sub{font-size:13px;color:var(--soft)}
.facts{margin:0 20px 6px;padding:10px 12px;border-radius:12px;background:#e9e4c6;display:grid;grid-template-columns:auto 1fr;gap:4px 12px;font-size:14px}
.facts dt{color:var(--soft)}.facts dd{margin:0;color:var(--text);overflow-wrap:anywhere;font-variant-numeric:tabular-nums}
.facts .skel{padding:2px 0;grid-column:1/-1}
.acts{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:8px;padding:12px 20px 20px}
.acts .btn{justify-content:flex-start}
.acts .btn.red{color:var(--bad)}
.close{position:absolute;right:12px;top:12px;width:34px;height:34px;border-radius:50%;border:0;background:#0000000d;color:var(--ink);display:grid;place-items:center;cursor:pointer;z-index:2}
.close:hover{background:#0000001a}
@media (max-width:560px){
dialog.sheet{margin:auto 0 0;width:100vw;max-width:100vw;border-radius:20px 20px 0 0;padding-bottom:env(safe-area-inset-bottom)}
dialog.sheet[open]{animation:sheetin .35s var(--ease)}
.pill{max-width:56%}
.keys{display:none}
}
@keyframes sheetin{from{transform:translateY(100%)}}
.viewer{width:min(64rem,calc(100vw - 16px));background:#141414;color:#fff}
.viewer .stage{position:relative;display:grid;place-items:center;min-height:50vh;padding:12px}
.viewer img{max-width:100%;max-height:72vh;border-radius:6px;transition:opacity .2s}
.viewer img.loading{opacity:0}
.viewer .stage .spin{position:absolute;border-color:#444;border-top-color:#fff}
.viewer .bar{display:flex;align-items:center;gap:8px;padding:10px 14px;border-top:1px solid #333;font-size:14px}
.viewer .bar span{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.viewer .nav{position:absolute;top:50%;transform:translateY(-50%);width:44px;height:44px;border-radius:50%;border:0;background:#fff2;color:#fff;display:grid;place-items:center;cursor:pointer}
.viewer .nav:hover{background:#fff4}
.viewer .prev{left:10px}.viewer .next{right:10px}
.viewer .close{background:#fff2;color:#fff}
.editor{width:min(56rem,calc(100vw - 16px))}
.editor .dbody{padding-bottom:0}
.editor h2{display:flex;gap:8px;align-items:center;padding-right:40px;overflow-wrap:anywhere}
.editor .dirty{width:9px;height:9px;border-radius:50%;background:var(--warn);opacity:0;transition:opacity .2s;flex:none}
.editor.changed .dirty{opacity:1}
.editor textarea{min-height:55vh;font-size:13px;tab-size:4}
.editor .row2{justify-content:space-between;align-items:center}
.hint{font-size:13px}
kbd{font:600 11px ui-monospace,monospace;background:#fffdf0;border:1px solid #d6cc8f;border-bottom-width:2px;border-radius:5px;padding:1px 5px}
@media (prefers-reduced-motion:reduce){*,*::before,*::after{animation:none!important;transition:none!important}}
</style></head><body><main>
<header class="head"><div class="brand"><a class="logo" href="https://ankush.one/3dsDL/" target="_blank" rel="noopener" title="3dsDL website"><img id="logo" alt=""><h1>3dsDL</h1></a>
<nav class="ext" id="ext"><a href="https://ankush.one/3dsDL/" target="_blank" rel="noopener">Website</a><a href="https://github.com/ankushKun/3dsDL" target="_blank" rel="noopener">GitHub</a></nav></div><div class="pill" id="pill" data-state="connecting" role="status" aria-live="polite"><i class="dot"></i><span id="pilltext">Connecting...</span></div></header>
<nav class="tabs" id="tabs"><span class="ink" id="ink"></span><a href="#links" id="t-links" class="on">Links</a><a href="#files:%2F" id="t-files">Files</a></nav>
<div class="banner" id="offline" hidden><span class="spin"></span><span>Can't reach the 3DS. Keep 3dsDL open on Receive; this page reconnects by itself.</span></div>
<section id="links">
)PAGE";

const char *const PageEnd = R"PAGE(<form method="post" action="/send" class="panel" id="sendform">
<textarea name="links" id="linkbox" placeholder="https://..." autofocus required></textarea>
<div class="formrow"><span class="count" id="linkcount"></span><button class="btn primary" type="submit" id="sendbtn">Send to 3DS</button></div></form>
<ul class="sent" id="sent"></ul>
</section>
<section id="files" hidden>
<div id="gate" class="panel gate" hidden></div>
<div id="browser" hidden>
<div class="card" id="ccard" hidden></div>
<div class="storage"><div class="meter" id="meter"><i></i></div><span id="free"></span></div>
<div class="pathbar"><button class="btn icon-only" id="up" title="Up one folder (Backspace)" aria-label="Up one folder"></button><nav class="crumbs" id="crumbs"></nav></div>
<div class="tools">
<label class="btn primary" title="Upload files (U)"><span id="ic-upload"></span>Upload<input type="file" id="pick" multiple hidden></label>
<label class="btn" id="pickdirbtn"><span id="ic-folder"></span>Folder<input type="file" id="pickdir" webkitdirectory multiple hidden></label>
<button class="btn" id="mkdir"></button>
<button class="btn icon-only reload" id="reload" title="Refresh" aria-label="Refresh"></button>
</div>
<div class="tools2">
<label class="search"><span id="ic-search"></span><input type="search" id="filter" placeholder="Filter this folder" autocomplete="off" aria-label="Filter this folder"></label>
<select id="sort" aria-label="Sort"><option value="name">By name</option><option value="size">By size</option><option value="time">By date</option><option value="type">By type</option></select>
<label class="check"><input type="checkbox" id="hidden">Hidden</label>
</div>
<div class="ups panel" id="ups" hidden>
<div class="uphead"><div><b id="uptitle"></b><span id="upsub"></span></div><div class="upbtns"><button class="btn small toggle" id="uptoggle" aria-expanded="false"></button><button class="btn small" id="upcancel">Cancel All</button><button class="btn small" id="upclear">Clear Finished</button></div><div class="track"><div class="fill" id="upfill"></div></div></div>
<div class="updetail"><ul id="uplist"></ul>
<label class="check" style="margin-top:10px"><input type="checkbox" id="offer" checked>Offer to install CIAs on the 3DS once they arrive</label></div></div>
<div class="listwrap"><div class="loadline" id="loadline"></div>
<div class="listhead"><input type="checkbox" id="all" aria-label="Select all"><span id="count"></span></div>
<ul class="list" id="list"></ul></div>
<small class="hint"><span class="keys"><kbd>/</kbd> filter, <kbd>Backspace</kbd> up, <kbd>Del</kbd> delete, <kbd>F2</kbd> rename, <kbd>Ctrl</kbd>+<kbd>A</kbd> select all.</span></small>
</div>
</section>
</main>
<div class="dock">
<div class="selbar" id="selbar" hidden><b id="selcount"></b><button class="btn small" id="selinstall">Install on 3DS</button><button class="btn small" id="selmove">Move</button><button class="btn small danger" id="seldel">Delete</button><button class="btn small" id="selnone">Clear</button></div>
</div>
<div class="drop" id="drop" hidden><div><span id="ic-drop"></span><b>Drop to upload</b><span id="dropto"></span></div></div>
<div class="toasts" id="toasts"></div>
<dialog id="dlg"><div class="dbody"><h2 id="dt"></h2><div class="warnbox" id="dw" hidden><span id="ic-warn"></span><span></span></div><p id="dp"></p>
<input type="text" id="di" autocomplete="off" spellcheck="false"><p class="err" id="de" hidden></p></div><div class="row2" id="db"></div></dialog>
<dialog id="sheet" class="sheet"></dialog>
<dialog id="viewer" class="viewer"><button class="close" id="vclose" aria-label="Close"></button><div class="stage"><span class="spin" id="vspin"></span><img id="vimg" alt=""><button class="nav prev" id="vprev" aria-label="Previous"></button><button class="nav next" id="vnext" aria-label="Next"></button></div>
<div class="bar"><span id="vname"></span><a class="btn small" id="vdl" download>Download</a></div></dialog>
<dialog id="editor" class="editor"><button class="close" id="eclose" aria-label="Close"></button><div class="dbody"><h2><span id="ename"></span><i class="dirty" title="Not saved"></i></h2>
<textarea id="etext" spellcheck="false"></textarea><p class="err" id="eerr" hidden></p></div>
<div class="row2"><span class="muted" id="einfo"></span><span><button class="btn" id="ecancel">Close</button> <button class="btn primary" id="esave">Save</button></span></div></dialog>
<script>
'use strict';
const $ = s => document.querySelector(s);
const enc = encodeURIComponent;
const H = {'X-Requested-With': '3dsDL'};
const esc = t => String(t).replace(/[&<>"]/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));
const store = {
  get(key, fallback) { try { const v = localStorage.getItem('3dsdl.' + key); return v === null ? fallback : JSON.parse(v); } catch (e) { return fallback; } },
  set(key, value) { try { localStorage.setItem('3dsdl.' + key, JSON.stringify(value)); } catch (e) {} }
};

// --- Icons ---------------------------------------------------------------------
const ICONS = {
  upload: '<path d="M12 16V4M7 9l5-5 5 5M4 20h16"/>',
  folder: '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>',
  folderplus: '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2zM12 10v6M9 13h6"/>',
  refresh: '<path d="M20 11a8 8 0 1 0-2.3 5.7M20 4v7h-7"/>',
  search: '<circle cx="11" cy="11" r="7"/><path d="m20 20-4-4"/>',
  download: '<path d="M12 4v12M7 11l5 5 5-5M4 20h16"/>',
  eye: '<path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>',
  edit: '<path d="M4 20h4L19 9l-4-4L4 16zM14 6l4 4"/>',
  install: '<path d="M12 3v10M8 9l4 4 4-4M4 15v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3"/>',
  copy: '<rect x="9" y="9" width="11" height="11" rx="2"/><path d="M5 15V6a2 2 0 0 1 2-2h8"/>',
  rename: '<path d="M4 7V5h16v2M12 5v14M9 19h6"/>',
  move: '<path d="M5 12h14M13 6l6 6-6 6"/>',
  trash: '<path d="M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13"/>',
  x: '<path d="M6 6l12 12M18 6 6 18"/>',
  check: '<path d="M5 12l5 5L20 7"/>',
  up: '<path d="M12 19V5M5 12l7-7 7 7"/>',
  warn: '<path d="M12 3 2 21h20zM12 10v4M12 17.5v.5"/>',
  info: '<circle cx="12" cy="12" r="9"/><path d="M12 11v6M12 7.5v.5"/>',
  link: '<path d="M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1"/>',
  lock: '<rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/>',
  left: '<path d="M15 5l-7 7 7 7"/>',
  right: '<path d="M9 5l7 7-7 7"/>',
  console: '<rect x="6" y="2.5" width="12" height="19" rx="2"/><path d="M8.5 5h7v5h-7zM10 15h4M12 13v4"/>',
  retry: '<path d="M4 4v6h6M4 10a8 8 0 1 1 2.3 5.7"/>',
  chevron: '<path d="M6 9l6 6 6-6"/>',
  out: '<path d="M7 17 17 7M9 7h8v8"/>',
  more: '<circle cx="5" cy="12" r="1.7"/><circle cx="12" cy="12" r="1.7"/><circle cx="19" cy="12" r="1.7"/>'
};
const svg = name => '<svg class="i' + (name === 'more' ? ' fill' : '') + '" viewBox="0 0 24 24" aria-hidden="true">' + ICONS[name] + '</svg>';
const FOLDER = '<svg viewBox="0 0 26 26" aria-hidden="true"><path d="M2 6.5A2.5 2.5 0 0 1 4.5 4h5.6l2.4 2.6h9A2.5 2.5 0 0 1 24 9.1V20a2.5 2.5 0 0 1-2.5 2.5h-17A2.5 2.5 0 0 1 2 20z" fill="#e6b422"/><path d="M2 10h22v10a2.5 2.5 0 0 1-2.5 2.5h-17A2.5 2.5 0 0 1 2 20z" fill="#f7d55c"/></svg>';
for (const [id, name] of [['ic-upload', 'upload'], ['ic-folder', 'folder'], ['ic-search', 'search'], ['ic-drop', 'upload'], ['ic-warn', 'warn']]) $('#' + id).outerHTML = svg(name);
$('#mkdir').innerHTML = svg('folderplus') + 'New Folder';
$('#logo').src = document.querySelector('link[rel=icon]').href;
for (const a of document.querySelectorAll('#ext a')) a.insertAdjacentHTML('beforeend', svg('out'));
$('#reload').innerHTML = svg('refresh');
$('#up').innerHTML = svg('up');
for (const [id, name] of [['vclose', 'x'], ['eclose', 'x'], ['vprev', 'left'], ['vnext', 'right']]) $('#' + id).innerHTML = svg(name);

// --- Paths and files -----------------------------------------------------------
const join = (dir, name) => dir === '/' ? '/' + name : dir + '/' + name;
const parent = p => p.slice(0, p.lastIndexOf('/')) || '/';
const base = p => p.slice(p.lastIndexOf('/') + 1);
const ext = n => { const i = n.lastIndexOf('.'); return i > 0 ? n.slice(i + 1).toLowerCase() : ''; };
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
const KINDS = {cia: 'cia', '3dsx': '3dsx', zip: 'zip', '7z': 'zip', rar: 'zip', png: 'img', jpg: 'img', jpeg: 'img', gif: 'img', bmp: 'img', webp: 'img',
  txt: 'txt', ini: 'txt', cfg: 'txt', json: 'txt', xml: 'txt', log: 'txt', md: 'txt', yml: 'txt', yaml: 'txt', lua: 'txt', nds: 'rom', '3ds': 'rom', cci: 'rom',
  firm: 'rom', tik: 'rom', smdh: 'rom', mp3: 'media', ogg: 'media', wav: 'media', mp4: 'media', bcstm: 'media'};
const TEXT = new Set(['txt', 'ini', 'cfg', 'json', 'xml', 'log', 'md', 'yml', 'yaml', 'lua', 'csv', 'conf', 'sh', 'py', 'js', 'html', 'htm', 'css', 'opt']);
const PICTURES = new Set(['png', 'jpg', 'jpeg', 'gif', 'bmp', 'webp']);
const MEDIA = new Set(['mp3', 'ogg', 'wav', 'mp4', 'pdf']);
const TITLED = new Set(['cia', '3dsx', 'smdh', '3ds', 'cci', 'nds']);
const fileUrl = (path, download) => '/api/file?' + (download ? 'download=1&' : '') + 'path=' + enc(path);
// Places where a slip can stop the console from starting or lose games.
function risk(paths) {
  for (const p of paths) {
    const l = p.toLowerCase();
    if (l === '/boot.firm' || l === '/luma' || l.startsWith('/luma/') || l === '/boot9strap' || l.startsWith('/boot9strap/'))
      return 'This is part of what starts custom firmware. Changing it can stop the 3DS from starting.';
    if (l === '/nintendo 3ds' || l.startsWith('/nintendo 3ds/'))
      return 'This folder holds installed software, saves and extra data. Changing it can break games.';
    if (l === '/boot.3dsx' || l === '/3ds/3dsdl' || l.startsWith('/3ds/3dsdl/'))
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

// --- Toasts and dialogs --------------------------------------------------------
function toast(text, kind = '') {
  const e = document.createElement('div');
  e.className = 'toast ' + kind;
  e.innerHTML = svg(kind === 'bad' ? 'warn' : kind === 'good' ? 'check' : 'info');
  e.append(text);
  $('#toasts').append(e);
  setTimeout(() => { e.classList.add('out'); setTimeout(() => e.remove(), 320); }, kind === 'bad' ? 5500 : 3800);
}
function fail(e) { if (e && e.message) toast(e.message, 'bad'); }
// Asks a question. With o.action the main button runs it, shows a spinner
// and keeps the dialog open with the error when it fails.
function ask(title, text, o = {}) {
  return new Promise(resolve => {
    const d = $('#dlg'), input = $('#di'), err = $('#de'), row = $('#db'), w = $('#dw');
    $('#dt').textContent = title;
    $('#dp').textContent = text || '';
    $('#dp').hidden = !text;
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
      b.className = 'btn' + (n === main ? (o.danger ? ' danger' : ' primary') : '');
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
function showStatus() {
  const p = $('#pill');
  const state = S.offline ? 'offline' : U.current ? 'sending' : S.state;
  const text = S.offline ? SAYS.offline : U.current ? U.pill : (S.text || SAYS[S.state] || S.state);
  p.dataset.state = state;
  $('#pilltext').textContent = text;
  p.title = text;
  const c = $('#ccard');
  const show = !S.offline && S.access === 'allowed' && !!HEADS[S.state];
  if (show) {
    const key = S.state + S.text;
    if (c.dataset.key !== key) {
      c.dataset.key = key;
      c.dataset.state = S.state;
      const art = S.state === 'installing' || S.state === 'downloading' ? '<span class="spin"></span>' : svg('console');
      c.innerHTML = '<div class="art">' + art + '</div><div><b>' + esc(HEADS[S.state]) + '</b><span>' + esc(S.text) + '</span></div>';
    }
  } else c.dataset.key = '';
  c.hidden = !show;
  for (const span of document.querySelectorAll('#sent li.wait span')) span.textContent = S.state === 'ready' ? 'Sent to the 3DS.' : (S.text || SAYS[S.state]);
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
    if (onFiles() && !$('#browser').hidden) load(F.cwd, true);
  }
  S.seq = j.resultSeq;
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
  else if (onFiles() && !$('#browser').hidden) { toast('Connected to the 3DS again.', 'good'); load(F.cwd, true); }
}
document.addEventListener('visibilitychange', () => { if (!document.hidden) poll(); });

// --- Tabs and routes ------------------------------------------------------------
const onFiles = () => location.hash.startsWith('#files');
// The highlight slides between tabs, but lands in place the first time.
let inked = false;
function ink() {
  const on = $('.tabs a.on'), i = $('#ink');
  if (!on) return;
  if (!inked) i.style.transition = 'none';
  i.style.left = on.offsetLeft + 'px';
  i.style.width = on.offsetWidth + 'px';
  if (!inked) { void i.offsetWidth; i.style.transition = ''; inked = true; }
}
addEventListener('resize', ink);
function route() {
  const files = onFiles();
  $('#links').hidden = files;
  $('#files').hidden = !files;
  $('#t-links').classList.toggle('on', !files);
  $('#t-files').classList.toggle('on', files);
  ink();
  if (!files) return;
  let path = '/';
  try { path = decodeURIComponent(location.hash.slice(7)) || '/'; } catch (e) {}
  $('#t-files').href = '#files:' + enc(path);
  if (!S.files) { gate('off'); return; }
  load(path);
}
const go = path => { location.hash = '#files:' + enc(path); };
addEventListener('hashchange', route);

// --- Links ------------------------------------------------------------------------
function linksIn(text) { return [...new Set((text.match(/https?:\/\/[^\s<>"'()]+/g) || []).map(l => l.replace(/[.,;]+$/, '')))]; }
function linkName(l) { try { return decodeURIComponent(base(new URL(l).pathname)) || new URL(l).host; } catch (e) { return l; } }
$('#sendform').noValidate = true;
$('#linkbox').addEventListener('input', () => {
  const n = linksIn($('#linkbox').value).length, c = $('#linkcount');
  c.textContent = n ? n + (n === 1 ? ' link' : ' links') + ' ready' : '';
  c.classList.toggle('has', n > 0);
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
  b.classList.add('busy');
  try {
    const j = await api('POST', '/send', {links: box.value});
    const li = document.createElement('li');
    li.className = 'wait';
    li.innerHTML = svg('link') + '<div><b></b><span>Sent to the 3DS.</span></div>';
    li.querySelector('b').textContent = j.count + (j.count === 1 ? ' link: ' : ' links: ') + links.slice(0, 2).map(linkName).join(', ') + (links.length > 2 ? ' and ' + (links.length - 2) + ' more' : '');
    $('#sent').prepend(li);
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
const F = {cwd: '/', entries: [], free: -1, total: -1, sel: new Set(), anchor: null, token: 0, fresh: new Set(), scrollTo: ''};
$('#sort').value = store.get('sort', 'name');
$('#hidden').checked = store.get('hidden', false);
if (/iPhone|iPad|iPod/.test(navigator.userAgent)) $('#pickdirbtn').hidden = true;
function gate(state) {
  S.access = state;
  if (!onFiles()) return;
  $('#browser').hidden = true;
  const g = $('#gate');
  g.hidden = false;
  g.className = 'panel gate' + (state === 'denied' || state === 'off' ? ' no' : '');
  if (state === 'pending') {
    g.innerHTML = '<div class="art">' + svg('lock') + '</div><h2>Allow this device on the 3DS</h2><p>The console is asking whether <b>' + esc(S.you || 'this device') +
      '</b> may see and change the files on its SD card. Choose <b>Allow</b> on the 3DS.</p><div class="waiting"><span class="spin"></span>Waiting for the 3DS</div>';
    poll();
  } else if (state === 'denied') {
    g.innerHTML = '<div class="art">' + svg('lock') + '</div><h2>Not allowed</h2><p>The 3DS chose not to let ' + esc(S.you || 'this device') + ' see its files. To ask again, leave Receive on the console and open it again.</p>';
  } else if (state === 'off') {
    g.innerHTML = '<div class="art">' + svg('lock') + '</div><h2>Files are not shared</h2><p>This 3DS only takes links.</p>';
  } else {
    g.innerHTML = '<div class="art">' + svg('console') + '</div><h2>Connecting to the 3DS</h2><div class="waiting"><span class="spin"></span>One moment</div>';
  }
}
function skeleton() {
  const ul = $('#list');
  ul.classList.remove('dim');
  ul.innerHTML = Array.from({length: 6}, (_, i) => '<li class="skel"><i></i><b style="--w:' + (35 + (i * 17) % 45) + '%"></b></li>').join('');
}
function loading(on) {
  $('#loadline').classList.toggle('on', on);
  $('#reload').classList.toggle('busy', on);
  $('#list').classList.toggle('dim', on && !!F.entries.length);
}
async function load(path, quietly, priority) {
  const token = ++F.token, moved = path !== F.cwd;
  F.cwd = path;
  if (moved) { F.sel.clear(); F.anchor = null; $('#filter').value = ''; }
  loading(true);
  if ($('#browser').hidden && !quietly && S.access !== 'pending') gate('connecting');
  const slow = setTimeout(() => { if (token === F.token && moved && !quietly) skeleton(); }, 180);
  try {
    const j = await api('GET', '/api/list?path=' + enc(path) + ($('#hidden').checked ? '&hidden=1' : ''), undefined, priority !== undefined ? priority : quietly ? 1 : 0);
    if (token !== F.token) return;
    Object.assign(F, {entries: j.entries, free: j.free, total: j.total});
    S.access = 'allowed';
    $('#gate').hidden = true;
    $('#browser').hidden = false;
    render(moved || !quietly);
    showStatus();
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
  const byName = new Map(F.entries.map(e => [e.name, e]));
  try {
    await timesOf(path, [...byName.keys()], 3, (got, part) => {
      if (token !== F.token) return false;
      for (const n of part) byName.get(n).time = got.get(n);
      for (const li of $('#list').children) if (li.entry && part.includes(li.entry.name)) li.querySelector('.meta').textContent = meta(li.entry);
    });
  } catch (e) {}
  if (token === F.token && $('#sort').value === 'time') render(false);
}
const meta = e => [e.dir ? 'Folder' : size(e.size), when(e.time)].filter(Boolean).join(' · ');
function sorted() {
  const by = $('#sort').value, byName = (a, b) => a.name.localeCompare(b.name, undefined, {numeric: true, sensitivity: 'base'});
  return F.entries.slice().sort((a, b) => a.dir !== b.dir ? (a.dir ? -1 : 1)
    : by === 'size' && !a.dir ? b.size - a.size || byName(a, b)
    : by === 'time' ? b.time - a.time || byName(a, b)
    : by === 'type' && !a.dir ? ext(a.name).localeCompare(ext(b.name)) || byName(a, b)
    : byName(a, b));
}
function shown() {
  const q = $('#filter').value.trim().toLowerCase();
  return sorted().filter(e => !q || e.name.toLowerCase().includes(q));
}
function marked(name) {
  const q = $('#filter').value.trim().toLowerCase();
  const at = q ? name.toLowerCase().indexOf(q) : -1;
  if (at < 0) return esc(name);
  return esc(name.slice(0, at)) + '<mark>' + esc(name.slice(at, at + q.length)) + '</mark>' + esc(name.slice(at + q.length));
}
function badge(e) {
  if (e.dir) return '<div class="ic k-dir">' + FOLDER + '</div>';
  const x = ext(e.name);
  return '<div class="ic k-' + (KINDS[x] || 'file') + '">' + esc((x || 'file').slice(0, 4).toUpperCase()) + '</div>';
}
let watcher = null;
function render(animate) {
  const cr = $('#crumbs');
  cr.textContent = '';
  const crumb = (label, to) => { const a = document.createElement('a'); a.href = '#files:' + enc(to); a.textContent = label; cr.append(a); };
  crumb('SD', '/');
  let p = '';
  for (const part of F.cwd.split('/').filter(Boolean)) { p += '/' + part; const s = document.createElement('span'); s.textContent = '/'; cr.append(s); crumb(part, p); }
  $('#up').disabled = F.cwd === '/';
  const meter = $('#meter');
  if (F.total > 0 && F.free >= 0) {
    const used = (F.total - F.free) / F.total;
    meter.firstChild.style.width = (used * 100).toFixed(1) + '%';
    meter.classList.toggle('full', used > .92);
    meter.hidden = false;
    $('#free').textContent = size(F.free) + ' free of ' + size(F.total);
  } else { meter.hidden = true; $('#free').textContent = F.free >= 0 ? size(F.free) + ' free' : ''; }
  const list = shown(), ul = $('#list');
  ul.textContent = '';
  ul.classList.remove('dim');
  if (watcher) watcher.disconnect();
  watcher = 'IntersectionObserver' in window ? new IntersectionObserver(seen, {rootMargin: '120px'}) : null;
  if (!list.length) {
    const li = document.createElement('li');
    li.className = 'empty';
    li.innerHTML = F.entries.length ? svg('search') + '<b>Nothing matches</b><span>Try another word, or clear the filter.</span>' : svg('folder') + '<b>This folder is empty</b><span>Drop files here, or use Upload.</span>';
    ul.append(li);
  }
  list.forEach((e, n) => {
    const path = join(F.cwd, e.name), li = document.createElement('li');
    li.className = 'row' + (F.sel.has(path) ? ' on' : '') + (animate ? ' anim' : '') + (F.fresh.has(path) ? ' fresh' : '');
    li.style.setProperty('--i', Math.min(n, 14));
    li.tabIndex = 0;
    li.dataset.path = path;
    li.innerHTML = '<input type="checkbox" aria-label="Select ' + esc(e.name) + '"' + (F.sel.has(path) ? ' checked' : '') + '>' + badge(e) +
      '<div class="what"><span class="name">' + marked(e.name) + '</span><span class="meta">' + esc(meta(e)) +
      '</span></div><button class="more" aria-label="Options for ' + esc(e.name) + '">' + svg('more') + '</button>';
    li.entry = e;
    ul.append(li);
    if (!e.dir && TITLED.has(ext(e.name))) { if (watcher) watcher.observe(li); else icon(li); }
  });
  F.fresh.clear();
  if (F.scrollTo) {
    const row = [...ul.children].find(r => r.dataset.path === F.scrollTo);
    F.scrollTo = '';
    if (row) reveal(row);
  }
  const n = F.entries.length;
  $('#count').textContent = list.length === n ? n + (n === 1 ? ' item' : ' items') : list.length + ' of ' + n + ' shown';
  selbar();
}
// Scrolls a row into the middle of the screen, unless it is in full view
// already (and clear of the selection bar at the bottom).
function reveal(row) {
  const ups = $('#ups'), r = row.getBoundingClientRect();
  const top = ups.hidden ? 0 : Math.max(0, ups.getBoundingClientRect().bottom);
  const bottom = innerHeight - ($('#selbar').hidden ? 0 : 80);
  if (r.top >= top && r.bottom <= bottom) return;
  const still = matchMedia('(prefers-reduced-motion: reduce)').matches;
  row.scrollIntoView({block: 'center', behavior: still ? 'auto' : 'smooth'});
}
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
async function icon(li, target) {
  const url = await iconFor(li.dataset.path, li.entry.time);
  const box = (target || li).querySelector('.ic');
  if (url && box) { box.className = 'ic img'; box.innerHTML = '<img alt="" src="' + url + '">'; }
}
function selbar() {
  const n = F.sel.size, all = $('#all'), visible = shown().length;
  $('#selbar').hidden = !n;
  $('#selcount').textContent = n + ' selected';
  $('#selinstall').hidden = ![...F.sel].some(p => ext(p) === 'cia');
  all.checked = n > 0 && n >= visible;
  all.indeterminate = n > 0 && n < visible;
}
function select(path, on) {
  if (on) F.sel.add(path); else F.sel.delete(path);
  const li = [...$('#list').children].find(r => r.dataset.path === path);
  if (li) { li.classList.toggle('on', on); li.querySelector('input').checked = on; }
}
function selectAll(on) { for (const e of shown()) select(join(F.cwd, e.name), on); selbar(); }
$('#all').onchange = e => selectAll(e.target.checked);
$('#list').addEventListener('click', e => {
  const li = e.target.closest('.row');
  if (!li) return;
  const path = li.dataset.path;
  if (e.target.matches('input')) {
    const rows = [...$('#list').querySelectorAll('.row')];
    if (e.shiftKey && F.anchor) {
      const a = rows.findIndex(r => r.dataset.path === F.anchor), b = rows.indexOf(li);
      if (a >= 0) for (const r of rows.slice(Math.min(a, b), Math.max(a, b) + 1)) select(r.dataset.path, e.target.checked);
    } else select(path, e.target.checked);
    F.anchor = path;
    selbar();
    return;
  }
  if (e.target.closest('.more')) return details(li);
  if (li.entry.dir) go(path); else details(li);
});
$('#list').addEventListener('keydown', e => {
  const li = e.target.closest('.row');
  if (!li || e.target !== li) return;
  if (e.key === 'Enter') { e.preventDefault(); li.click(); }
  if (e.key === ' ') { e.preventDefault(); li.querySelector('input').click(); }
  if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
    e.preventDefault();
    const next = e.key === 'ArrowDown' ? li.nextElementSibling : li.previousElementSibling;
    if (next && next.classList.contains('row')) next.focus();
  }
});
$('#filter').oninput = () => render(false);
$('#sort').onchange = () => { store.set('sort', $('#sort').value); render(true); };
$('#hidden').onchange = () => { store.set('hidden', $('#hidden').checked); load(F.cwd, true); };
$('#reload').onclick = () => load(F.cwd, true);
$('#up').onclick = () => { if (F.cwd !== '/') go(parent(F.cwd)); };
$('#selnone').onclick = () => selectAll(false);
$('#seldel').onclick = () => remove([...F.sel]);
$('#selmove').onclick = () => move([...F.sel]);
$('#selinstall').onclick = () => install([...F.sel]);

// The details sheet: what a file is, and everything that can be done with it.
async function details(li) {
  const e = li.entry, path = li.dataset.path, x = ext(e.name), d = $('#sheet');
  const acts = [];
  const add = (icon, label, fn, cls = '') => acts.push({icon, label, fn, cls});
  if (e.dir) add('folder', 'Open', () => go(path));
  else {
    add('download', 'Download', () => download(path, e));
    if (PICTURES.has(x)) add('eye', 'View', () => view(path));
    else if (MEDIA.has(x)) add('eye', 'Open', () => window.open(fileUrl(path), '_blank'));
    if (TEXT.has(x) && e.size <= 1048576) add('edit', 'Edit', () => edit(path));
    if (x === 'cia') add('install', 'Install on 3DS', () => install([path]));
  }
  add('rename', 'Rename', () => rename(path));
  add('move', 'Move To...', () => move([path]));
  add('copy', 'Copy Path', () => copy(path));
  add('trash', 'Delete', () => remove([path]), ' red');
  const facts = [[e.dir ? 'Kind' : 'Size', e.dir ? 'Folder' : size(e.size) + (e.size >= 1024 ? ' (' + e.size.toLocaleString() + ' bytes)' : '')], ['Changed', when(e.time) || 'Unknown'], ['Where', parent(path)]];
  d.innerHTML = '<button class="close" aria-label="Close">' + svg('x') + '</button><div class="top">' + badge(e) + '<div><h2></h2><div class="sub"></div></div></div><dl class="facts">' +
    facts.map(([k, v]) => '<dt>' + esc(k) + '</dt><dd>' + esc(v) + '</dd>').join('') + '</dl><div class="acts"></div>';
  d.querySelector('h2').textContent = e.name;
  d.querySelector('.sub').textContent = e.dir ? 'Folder' : (x ? x.toUpperCase() + ' file' : 'File');
  const box = d.querySelector('.acts');
  for (const a of acts) {
    const b = document.createElement('button');
    b.className = 'btn' + a.cls;
    b.innerHTML = svg(a.icon) + esc(a.label);
    b.onclick = () => { d.close(); a.fn(); };
    box.append(b);
  }
  d.querySelector('.close').onclick = () => d.close();
  d.onclick = ev => { if (ev.target === d) d.close(); };
  d.showModal();
  if (!e.dir && TITLED.has(x)) {
    icon(li, d.querySelector('.top'));
    const f = d.querySelector('.facts');
    f.insertAdjacentHTML('afterbegin', '<div class="skel"><b style="--w:70%"></b></div><div class="skel"><b style="--w:45%"></b></div>');
    try {
      const m = await api('GET', '/api/meta?path=' + enc(path));
      for (const s of f.querySelectorAll('.skel')) s.remove();
      const version = m.versionText || '';
      const rows = [['Title', m.name], ['Publisher', m.publisher], ['Product', m.product], ['Title ID', m.titleId], ['Version', version], ['Note', m.note]].filter(r => r[1]);
      f.insertAdjacentHTML('afterbegin', rows.map(([k, v]) => '<dt>' + esc(k) + '</dt><dd>' + esc(v) + '</dd>').join(''));
      if (m.name) d.querySelector('.sub').textContent = m.name + (m.publisher ? ' · ' + m.publisher : '');
    } catch (err) { for (const s of f.querySelectorAll('.skel')) s.remove(); }
  }
}
function download(path, e) {
  if (e.size > 64 * 1048576) toast('Downloading ' + e.name + '. The 3DS answers nothing else until it finishes.');
  const a = document.createElement('a');
  a.href = fileUrl(path, true);
  a.download = e.name;
  document.body.append(a);
  a.click();
  a.remove();
}
function copy(text) {
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
    F.fresh.add(join(parent(path), name));
    return name;
  }});
  if (typeof r === 'string') { toast('Renamed to ' + r, 'good'); load(F.cwd, true); }
}
async function move(paths) {
  const what = paths.length > 1 ? paths.length + ' items' : '"' + base(paths[0]) + '"';
  const r = await ask('Move ' + what, 'Type the folder to move to, starting with /.', {input: F.cwd, buttons: ['Cancel', 'Move'], warn: risk(paths), action: async to => {
    const dest = ('/' + to.trim()).replace(/\/+/g, '/').replace(/(.)\/$/, '$1');
    if (dest === F.cwd) throw new Error('They are already in ' + dest + '.');
    for (const p of paths) await api('POST', '/api/move', {from: p, to: join(dest, base(p))});
    return dest;
  }});
  if (typeof r === 'string') { F.sel.clear(); toast('Moved ' + what + ' to ' + r, 'good'); load(F.cwd, true); }
}
async function remove(paths) {
  const many = paths.length > 1;
  const ok = await ask('Delete ' + (many ? paths.length + ' items' : '"' + base(paths[0]) + '"') + '?', 'Deleted files and folders cannot be brought back.',
    {buttons: ['Cancel', 'Delete'], danger: true, warn: risk(paths), action: async () => { for (const p of paths) await api('POST', '/api/delete', {path: p}, 0, {retry: true}); }});
  if (ok !== true) return;
  for (const r of $('#list').querySelectorAll('.row')) if (paths.includes(r.dataset.path)) r.classList.add('gone');
  F.sel.clear();
  selbar();
  toast(many ? 'Deleted ' + paths.length + ' items' : 'Deleted ' + base(paths[0]), 'good');
  setTimeout(() => load(F.cwd, true), 280);
}
async function install(paths) {
  const cias = paths.filter(p => ext(p) === 'cia');
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
$('#mkdir').onclick = async () => {
  const r = await ask('New Folder', null, {input: '', buttons: ['Cancel', 'Create'], action: async name => {
    name = name.trim();
    if (!name) throw new Error('Type a name for the folder.');
    await api('POST', '/api/folder', {path: join(F.cwd, name)}, 0, {retry: true});
    F.fresh.add(join(F.cwd, name));
  }});
  if (r === true) load(F.cwd, true);
};

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
  $('#vname').textContent = base(path) + (V.list.length > 1 ? '  ·  ' + (V.at + 1) + ' of ' + V.list.length : '');
  $('#vdl').href = fileUrl(path, true);
  $('#vprev').hidden = $('#vnext').hidden = V.list.length < 2;
}
const step = n => { V.at = (V.at + n + V.list.length) % V.list.length; showPicture(); };
$('#vprev').onclick = () => step(-1);
$('#vnext').onclick = () => step(1);
$('#vclose').onclick = () => $('#viewer').close();
$('#viewer').addEventListener('keydown', e => { if (e.key === 'ArrowLeft') step(-1); if (e.key === 'ArrowRight') step(1); });
let touchX = null;
$('#viewer').addEventListener('touchstart', e => { touchX = e.touches[0].clientX; }, {passive: true});
$('#viewer').addEventListener('touchend', e => {
  if (touchX === null) return;
  const dx = e.changedTouches[0].clientX - touchX;
  if (Math.abs(dx) > 50 && V.list.length > 1) step(dx < 0 ? 1 : -1);
  touchX = null;
});

// The text editor.
const E = {path: '', saved: ''};
async function edit(path) {
  let text;
  try {
    const r = await run(() => call('GET', fileUrl(path), undefined, {raw: true}));
    if (!r.ok) throw new Error('Could not open ' + base(path) + '.');
    text = await r.text();
  } catch (e) { return fail(e); }
  Object.assign(E, {path, saved: text});
  $('#ename').textContent = base(path);
  $('#etext').value = text;
  $('#eerr').hidden = true;
  editInfo();
  $('#editor').showModal();
  $('#etext').focus();
  $('#etext').setSelectionRange(0, 0);
}
function editInfo() {
  const t = $('#etext').value;
  $('#editor').classList.toggle('changed', t !== E.saved);
  $('#einfo').textContent = t.split('\n').length + ' lines · ' + size(new Blob([t]).size) + (t !== E.saved ? ' · not saved' : '');
}
$('#etext').addEventListener('input', editInfo);
async function save() {
  const b = $('#esave'), text = $('#etext').value;
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
  if ($('#etext').value !== E.saved && await ask('Close without saving?', 'The changes to ' + base(E.path) + ' will be lost.', {buttons: ['Keep Editing', 'Close'], danger: true}) !== 1) return;
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
    const names = clash.slice(0, 5).map(f => base(f.path)).join('\n') + (clash.length > 5 ? '\n...and ' + (clash.length - 5) + ' more' : '');
    const stop = {done: false};
    const c = await ask(clash.length === 1 ? 'Replace this file?' : 'Replace ' + clash.length + ' files?',
      names,
      {buttons: ['Cancel', 'Skip', 'Replace all', 'Replace changed'], warn: risk(clash.map(f => f.path)), action: () => changed(clash, there, stop)});
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
    li.innerHTML = '<span class="n"></span><div class="track"><div class="fill"></div></div><span class="s">Waiting</span><div></div>';
    li.querySelector('.n').textContent = f.path;
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
  const box = f.li.lastChild;
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
function progress() {
  const live = U.items.filter(f => f.state !== 'off' && f.state !== 'failed');
  const left = live.filter(f => f.state === 'wait' || f.state === 'up');
  const panel = $('#ups');
  if (!U.items.length) { panel.hidden = true; return; }
  panel.hidden = false;
  $('#uptoggle').innerHTML = svg('chevron') + 'Details (' + U.items.length + ')';
  if (!left.length) {
    U.current = null;
    U.sent = 0;
    U.ms = 0;
    const failed = U.items.length - live.length;
    panel.classList.add('done');
    $('#uptitle').textContent = !live.length ? 'Uploads stopped'
      : live.length === 1 ? 'Upload finished' : 'All ' + live.length + ' uploads finished';
    $('#upsub').textContent = failed ? failed + ' did not finish' : size(live.reduce((n, f) => n + f.file.size, 0)) + ' sent';
    $('#upfill').style.width = live.length ? '100%' : '0';
    $('#upcancel').hidden = true;
    $('#upclear').hidden = false;
    showStatus();
    return;
  }
  panel.classList.remove('done');
  $('#upcancel').hidden = false;
  $('#upclear').hidden = left.length === U.items.length;
  const total = live.reduce((n, f) => n + f.file.size, 0), done = live.reduce((n, f) => n + (f.state === 'done' ? f.file.size : f.loaded), 0);
  const rate = U.ms > 1000 ? U.sent / (U.ms / 1000) : 0, at = live.length - left.length + 1;
  const pct = total ? Math.floor(100 * done / total) : 0;
  const now = U.current || left[0];
  $('#uptitle').textContent = 'Uploading ' + base(now.path) + (live.length > 1 ? ' (' + Math.min(at, live.length) + ' of ' + live.length + ')' : '');
  $('#upsub').textContent = [pct + '%', rate ? size(rate) + '/s' : '', rate ? duration((total - done) / rate) + ' left' : ''].filter(Boolean).join(' · ');
  $('#upfill').style.width = pct + '%';
  U.pill = 'Sending to the 3DS · ' + pct + '%';
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
      status(f, size(e.loaded) + ' of ' + size(e.total) + (s > 1 ? ' · ' + size(e.loaded / s) + '/s' : ''));
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
        if (ext(f.path) === 'cia') U.arrived.push(f.path);
        const top = f.path.startsWith(F.cwd === '/' ? '/' : F.cwd + '/') ? f.path.slice(F.cwd === '/' ? 1 : F.cwd.length + 1).split('/')[0] : '';
        if (top && !F.entries.some(e => e.name === top)) {
          // Shown now, ahead of the next upload, rather than once all are
          // done. Something already listed waits for the end: each listing
          // keeps the console from the next upload.
          F.fresh.add(join(F.cwd, top));
          F.scrollTo = join(F.cwd, top);
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
function openDetails(open) {
  $('#ups').classList.toggle('open', open);
  $('#uptoggle').setAttribute('aria-expanded', String(open));
  store.set('uploadDetails', open);
}
openDetails(store.get('uploadDetails', false));
$('#uptoggle').onclick = () => openDetails(!$('#ups').classList.contains('open'));
addEventListener('scroll', () => {
  const ups = $('#ups');
  if (!ups.hidden) ups.classList.toggle('stuck', ups.getBoundingClientRect().top <= 8.5 && scrollY > 0);
}, {passive: true});
$('#upcancel').onclick = () => {
  for (const f of U.items) if (f.state === 'wait') { f.state = 'off'; status(f, 'Cancelled', 'err'); buttons(f); }
  if (U.current && U.current.xhr) U.current.xhr.abort();
  progress();
};
$('#upclear').onclick = () => {
  U.items = U.items.filter(f => { const keep = f.state === 'wait' || f.state === 'up'; if (!keep) f.li.remove(); return keep; });
  progress();
};
addEventListener('beforeunload', e => { if (U.items.some(f => f.state === 'wait' || f.state === 'up')) { e.preventDefault(); e.returnValue = ''; } });
const pick = list => plan([...list].map(file => ({file, path: join(F.cwd, file.webkitRelativePath || file.name)})));
$('#pick').onchange = e => { pick(e.target.files); e.target.value = ''; };
$('#pickdir').onchange = e => { pick(e.target.files); e.target.value = ''; };
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
let depth = 0;
const canDrop = () => onFiles() && !$('#browser').hidden && !document.querySelector('dialog[open]');
addEventListener('dragenter', e => {
  if (!canDrop() || !e.dataTransfer.types.includes('Files')) return;
  depth++;
  $('#dropto').textContent = 'into ' + (F.cwd === '/' ? 'the SD card' : F.cwd);
  $('#drop').hidden = false;
});
addEventListener('dragleave', () => { if (--depth <= 0) { depth = 0; $('#drop').hidden = true; } });
addEventListener('dragover', e => { if (canDrop()) e.preventDefault(); });
addEventListener('drop', async e => {
  depth = 0;
  $('#drop').hidden = true;
  if (!canDrop()) return;
  e.preventDefault();
  // Each dropped item keeps its folders when the browser can walk it; one it
  // cannot walk goes up on its own, without taking the others' folders away.
  // The items are only readable until the first await.
  const into = F.cwd, out = [];
  const dropped = [...e.dataTransfer.items].filter(i => i.kind === 'file')
    .map(i => ({entry: i.webkitGetAsEntry && i.webkitGetAsEntry(), file: i.getAsFile()}));
  if (!dropped.some(d => d.entry)) return pick(e.dataTransfer.files);
  for (const d of dropped) {
    if (d.entry) await walk(d.entry, into, out);
    else if (d.file) out.push({file: d.file, path: join(into, d.file.name)});
  }
  plan(out, into);
});

// --- Keys ---------------------------------------------------------------------------
addEventListener('keydown', e => {
  if (!onFiles() || $('#browser').hidden || document.querySelector('dialog[open]')) return;
  const el = document.activeElement;
  const typing = /INPUT|TEXTAREA|SELECT/.test(el.tagName) && el.type !== 'checkbox';
  if (e.key === 'Escape') {
    if (el.id === 'filter') { $('#filter').value = ''; render(false); el.blur(); } else if (F.sel.size) selectAll(false);
    return;
  }
  if (typing) return;
  if (e.key === '/') { e.preventDefault(); $('#filter').focus(); }
  else if (e.key === 'Backspace' || (e.altKey && e.key === 'ArrowUp')) { e.preventDefault(); if (F.cwd !== '/') go(parent(F.cwd)); }
  else if (e.key === 'Delete' && F.sel.size) remove([...F.sel]);
  else if (e.key === 'F2' && F.sel.size === 1) { e.preventDefault(); rename([...F.sel][0]); }
  else if ((e.ctrlKey || e.metaKey) && e.key === 'a') { e.preventDefault(); selectAll(true); }
  else if (e.key === 'u' && !e.ctrlKey && !e.metaKey) $('#pick').click();
});

route();
requestAnimationFrame(ink);
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
