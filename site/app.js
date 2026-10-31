// Progressive enhancements for the 3dsDL website. The page works without
// this file except for the "send a link" QR tool. Everything here plays
// along with the DS and 3DS theme: the HOME Menu features, the status bar
// clock, PictoChat and Notifications behaviour, and the menu sounds. Nothing
// is hidden until the script has confirmed it is running, and motion is
// skipped when reduced motion is requested.

// Analytics: PostHog, in the same project as the rest of ankush.one and through the same first-party
// proxy (p1.ankush.one). This site shares the ankush.one origin with the others, so a visitor keeps one
// id and one replay session across all of them; the `site` property tells the events apart. Nothing
// loads on localhost or when the browser sends Global Privacy Control; window.track() and
// window.trackError() are safe to call either way.
(function () {
  !function(t,e){var o,n,p,r;e.__SV||(window.posthog && window.posthog.__loaded)||(window.posthog=e,e._i=[],e.init=function(i,s,a){function g(t,e){var o=e.split(".");2==o.length&&(t=t[o[0]],e=o[1]),t[e]=function(){t.push([e].concat(Array.prototype.slice.call(arguments,0)))}}(p=t.createElement("script")).type="text/javascript",p.crossOrigin="anonymous",p.async=!0,p.src=s.api_host.replace(".i.posthog.com","-assets.i.posthog.com")+"/static/array.js",(r=t.getElementsByTagName("script")[0]).parentNode.insertBefore(p,r);var u=e;for(void 0!==a?u=e[a]=[]:a="posthog",u.people=u.people||[],u.toString=function(t){var e="posthog";return"posthog"!==a&&(e+="."+a),t||(e+=" (stub)"),e},u.people.toString=function(){return u.toString(1)+".people (stub)"},o="ki Ci init qi Hi pr Bi zi Di capture calculateEventProperties Qi register register_once register_for_session unregister unregister_for_session Ki getFeatureFlag getFeatureFlagPayload getFeatureFlagResult getAllFeatureFlags isFeatureEnabled reloadFeatureFlags updateFlags updateEarlyAccessFeatureEnrollment getEarlyAccessFeatures on onFeatureFlags onSurveysLoaded onSessionId getSurveys getActiveMatchingSurveys renderSurvey displaySurvey cancelPendingSurvey canRenderSurvey canRenderSurveyAsync Xi identify setPersonProperties unsetPersonProperties group resetGroups setPersonPropertiesForFlags resetPersonPropertiesForFlags setGroupPropertiesForFlags resetGroupPropertiesForFlags reset shutdown setIdentity clearIdentity get_distinct_id getGroups get_session_id get_session_replay_url alias set_config startSessionRecording stopSessionRecording sessionRecordingStarted captureException addExceptionStep captureLog startExceptionAutocapture stopExceptionAutocapture loadToolbar get_property getSessionProperty Ji Gi createPersonProfile setInternalOrTestUser Yi Ai rn opt_in_capturing opt_out_capturing has_opted_in_capturing has_opted_out_capturing get_explicit_consent_status is_capturing clear_opt_in_out_capturing Vi debug mr it getPageViewId captureTraceFeedback captureTraceMetric Oi".split(" "),n=0;n<o.length;n++)g(u,o[n]);e._i.push([i,s,a])},e.__SV=1)}(document,window.posthog||[]);

  var SITE = "3dsDL";

  // The stub only gets its capture methods when init() runs, after the page has loaded, so anything
  // tracked before then waits here (with the time it happened) and is passed on by init().
  var early = [];
  function call(method, a, b) {
    if (early) early.push([method, a, b, { timestamp: new Date() }]);
    else if (typeof window.posthog[method] === "function") window.posthog[method](a, b);
  }
  // Analytics events must never break the page.
  window.track = function (name, props) {
    try { call("capture", name, props || {}); } catch (e) {}
  };
  window.trackError = function (err, context) {
    try { call("captureException", err, { context: context }); } catch (e) {}
  };

  var host = location.hostname;
  var off = host === "localhost" || host === "127.0.0.1" || host === "[::1]" || host === "" ||
    navigator.globalPrivacyControl === true;
  if (off) {
    early = null;
    return;
  }

  function init() {
    window.posthog.init("phc_nBvIMuMmaqQkAXL36Bi6eyTRga0A04klevYFyZf4cfc", {
      api_host: "https://p1.ankush.one",
      ui_host: "https://eu.posthog.com",
      defaults: "2026-05-30",
      // localStorage + cookie, so return visits join up. Every visitor gets a person profile.
      person_profiles: "always",
      capture_pageview: "history_change",
      capture_pageleave: true,
      autocapture: { capture_copied_text: true },
      capture_dead_clicks: true,
      capture_heatmaps: true,
      rageclick: true,
      capture_performance: { network_timing: true, web_vitals: true },
      capture_exceptions: true,
      // Session replay, with console logs and network calls. Headers, bodies and canvas capture also
      // need the matching switches on in the PostHog project's Session replay settings.
      disable_session_recording: false,
      enable_recording_console_log: true,
      session_recording: {
        maskAllInputs: false,
        maskInputOptions: { password: true },
        recordHeaders: true,
        recordBody: true,
        collectFonts: true
      },
      // Set per event rather than with register(): a registered property would be saved in the
      // storage this origin shares and follow the visitor onto the other sites.
      before_send: function (event) {
        if (event && event.properties) event.properties.site = SITE;
        return event;
      }
    });
    var held = early;
    early = null;
    held.forEach(function (c) {
      try { window.posthog[c[0]](c[1], c[2], c[0] === "capture" ? c[3] : undefined); } catch (e) {}
    });
  }

  // The library (~105 KB) waits until the page has loaded and the browser is idle.
  function whenIdle() {
    if (window.requestIdleCallback) requestIdleCallback(init, { timeout: 3000 });
    else setTimeout(init, 1);
  }
  if (document.readyState === "complete") whenIdle();
  else window.addEventListener("load", whenIdle);
})();

(function () {
  "use strict";

  var root = document.documentElement;
  root.classList.add("js");

  var REPO = "ankushKun/3dsDL";
  var PAGE_URL = location.origin + location.pathname;
  var reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  var SPRING = "cubic-bezier(0.34, 1.56, 0.64, 1)";
  var track = window.track;

  // Any part of the page can ask for a menu sound by name; setUpSounds fills
  // this in, and until then (or with sounds off) it does nothing.
  var sfx = { play: function () {} };

  function formatBytes(bytes) {
    if (bytes < 1024) return Math.round(bytes) + " B";
    var units = ["KiB", "MiB", "GiB"];
    var value = bytes / 1024;
    var unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
      value /= 1024;
      unit += 1;
    }
    return value.toFixed(value < 10 ? 2 : 1) + " " + units[unit];
  }

  // Latest release: the version on the hero sticker, and the file sizes.
  function showRelease() {
    fetch("https://api.github.com/repos/" + REPO + "/releases/latest", {
      headers: { Accept: "application/vnd.github+json" }
    })
      .then(function (response) {
        if (!response.ok) throw new Error("HTTP " + response.status);
        return response.json();
      })
      .then(function (release) {
        document.querySelectorAll("[data-release-tag]").forEach(function (element) {
          element.textContent = release.tag_name;
          element.hidden = false;
        });
        (release.assets || []).forEach(function (asset) {
          document
            .querySelectorAll('[data-asset-size="' + asset.name + '"]')
            .forEach(function (element) {
              element.textContent = formatBytes(asset.size);
            });
        });
      })
      .catch(function () {
        // Leave the static text in place when GitHub can't be reached.
      });
  }

  // Downloads of every file in every release, pre-releases included: the
  // same sum as the shields.io badge in the README. The total is kept for an
  // hour, so a returning visitor sees it at once and the page spends as few
  // of GitHub's 60 requests an hour as it can.
  function showDownloads() {
    var targets = document.querySelectorAll("[data-downloads]");
    if (!targets.length || !window.fetch) return;
    var key = "downloads:" + REPO;
    var show = function (total) {
      targets.forEach(function (element) {
        element.textContent = total.toLocaleString("en") + (total === 1 ? " download" : " downloads");
        element.hidden = false;
      });
    };
    var saved = null;
    try {
      saved = JSON.parse(localStorage.getItem(key));
    } catch (error) {}
    if (saved && typeof saved.total === "number") {
      show(saved.total);
      if (Date.now() - saved.at < 60 * 60 * 1000) return;
    }
    var total = 0;
    var page = function (number) {
      return fetch("https://api.github.com/repos/" + REPO + "/releases?per_page=100&page=" + number, {
        headers: { Accept: "application/vnd.github+json" }
      })
        .then(function (response) {
          if (!response.ok) throw new Error("HTTP " + response.status);
          return response.json();
        })
        .then(function (releases) {
          releases.forEach(function (release) {
            (release.assets || []).forEach(function (asset) {
              total += asset.download_count || 0;
            });
          });
          if (releases.length === 100) return page(number + 1);
        });
    };
    page(1)
      .then(function () {
        show(total);
        try {
          localStorage.setItem(key, JSON.stringify({ total: total, at: Date.now() }));
        } catch (error) {}
      })
      .catch(function () {
        // Keep the saved count, or nothing, when GitHub can't be reached.
      });
  }

  function copyText(text, button) {
    var label = button.querySelector("span");
    var original = label ? label.textContent : "";
    var done = function (message) {
      if (!label) return;
      label.textContent = message;
      setTimeout(function () {
        label.textContent = original;
      }, 1600);
    };
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(
        function () { done("Copied"); sfx.play("copy"); },
        function () { done("Copy failed"); }
      );
    } else {
      done("Copy failed");
    }
  }

  function setUpCopyButtons() {
    document.querySelectorAll("[data-copy]").forEach(function (button) {
      button.addEventListener("click", function () {
        var text = button.getAttribute("data-copy");
        copyText(text, button);
        track("download_link_copied", { file: text.split("/").pop() });
      });
    });
  }

  // Same rule 3dsDL applies: http(s), a host, and no spaces or control
  // characters.
  function checkLink(value) {
    if (!value) return { ok: false, message: "" };
    if (!/^https?:\/\//i.test(value)) {
      return { ok: false, message: "3dsDL only downloads links that start with http:// or https://." };
    }
    if (/[\s\u0000-\u001f\u007f]/.test(value)) {
      return { ok: false, message: "Links can't contain spaces. Copy the link again from its source." };
    }
    if (!/^https?:\/\/[^\/?#@\s]*@?[^\/?#\s]+/i.test(value)) {
      return { ok: false, message: "That link is missing a host name." };
    }
    if (/^http:\/\//i.test(value)) {
      return { ok: true, message: "Ready to scan. This link isn't encrypted, so 3dsDL will warn before downloading." };
    }
    return { ok: true, message: "Ready to scan. Open 3dsDL and choose Scan QR Code." };
  }

  function qrSvg(text, label) {
    var code = qrcode(0, "M");
    code.addData(text, "Byte");
    code.make();
    var count = code.getModuleCount();
    var border = 4;
    var size = count + border * 2;
    var path = "";
    for (var row = 0; row < count; row += 1) {
      for (var column = 0; column < count; column += 1) {
        if (code.isDark(row, column)) {
          path += "M" + (column + border) + " " + (row + border) + "h1v1h-1z";
        }
      }
    }
    var svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("viewBox", "0 0 " + size + " " + size);
    svg.setAttribute("shape-rendering", "crispEdges");
    svg.setAttribute("role", "img");
    svg.setAttribute("aria-label", label);
    var background = document.createElementNS(svg.namespaceURI, "rect");
    background.setAttribute("width", "100%");
    background.setAttribute("height", "100%");
    background.setAttribute("fill", "#fff");
    var modules = document.createElementNS(svg.namespaceURI, "path");
    modules.setAttribute("d", path);
    modules.setAttribute("fill", "#000");
    svg.appendChild(background);
    svg.appendChild(modules);
    return svg;
  }

  function setUpSendTool() {
    var input = document.getElementById("send-url");
    var status = document.getElementById("send-status");
    var preview = document.getElementById("send-qr");
    var reply = document.getElementById("send-reply");
    var bigButton = document.getElementById("send-big");
    var copyButton = document.getElementById("send-copy");
    var dialog = document.getElementById("qr-dialog");
    var dialogCode = document.getElementById("qr-dialog-code");
    var dialogUrl = document.getElementById("qr-dialog-url");
    if (!input || typeof qrcode !== "function") return;
    var placeholder = preview.innerHTML;
    var current = "";
    var timer = null;
    var showing = false;
    var source = "typed";

    // Analytics gets the link's host, never the whole link.
    function hostOf(link) {
      try {
        return new URL(link).hostname;
      } catch (error) {
        return "";
      }
    }

    // A new code pops in like a PictoChat message arriving.
    function arrive() {
      sfx.play("message");
      if (reduceMotion || !reply.animate) return;
      reply.animate(
        { transform: ["translateY(16px) scale(0.94)", "none"], opacity: [0.3, 1] },
        { duration: 520, easing: SPRING }
      );
    }

    // Encode non-ASCII characters as UTF-8, like the 3DS keyboard does.
    if (qrcode.stringToBytesFuncs && qrcode.stringToBytesFuncs["UTF-8"]) {
      qrcode.stringToBytes = qrcode.stringToBytesFuncs["UTF-8"];
    }

    function render() {
      var value = input.value.trim();
      var result = checkLink(value);
      status.textContent = result.message;
      status.className = "status" + (value ? (result.ok ? " ok" : " error") : "");
      current = result.ok ? value : "";
      bigButton.disabled = !result.ok;
      copyButton.disabled = !result.ok;
      preview.innerHTML = "";
      if (!result.ok) {
        preview.innerHTML = placeholder;
        showing = false;
        return;
      }
      try {
        preview.appendChild(qrSvg(value, "QR code for " + value));
      } catch (error) {
        preview.innerHTML = placeholder;
        status.textContent = "That link is too long for a QR code.";
        status.className = "status error";
        current = "";
        bigButton.disabled = true;
        copyButton.disabled = true;
      }
      if (current && !showing) {
        arrive();
        track("send_qr_made", { host: hostOf(current), https: /^https:/i.test(current), source: source });
      }
      showing = Boolean(current);
    }

    input.addEventListener("input", function () {
      clearTimeout(timer);
      timer = setTimeout(render, 120);
    });
    document.getElementById("send-form").addEventListener("submit", function (event) {
      event.preventDefault();
      render();
    });

    bigButton.addEventListener("click", function () {
      if (!current) return;
      dialogCode.innerHTML = "";
      dialogCode.appendChild(qrSvg(current, "Large QR code for " + current));
      dialogUrl.textContent = current;
      if (typeof dialog.showModal === "function") dialog.showModal();
      track("send_qr_enlarged", { host: hostOf(current) });
    });
    dialog.addEventListener("click", function (event) {
      if (event.target === dialog) dialog.close();
    });

    // A link to this page that opens with the same QR code.
    copyButton.addEventListener("click", function () {
      if (!current) return;
      copyText(PAGE_URL + "?url=" + encodeURIComponent(current) + "#send", copyButton);
      track("send_link_shared", { host: hostOf(current) });
    });

    var params = new URLSearchParams(location.search);
    var shared = params.get("url");
    if (shared) {
      // Out of the address bar before analytics starts, so the pageview
      // carries this page's address and not the link.
      params.delete("url");
      var rest = params.toString();
      history.replaceState(null, "", location.pathname + (rest ? "?" + rest : "") + location.hash);
      input.value = shared;
      source = "shared_link";
      render();
      source = "typed";
    }
  }

  // The 3DS's screens step through a download and install: each top and
  // bottom screenshot pair shows for a few seconds with its own doodled
  // notes, then the next.
  function setUpDeviceDemo() {
    var tops = document.querySelectorAll(".screen-top [data-hero-shot]");
    var bottoms = document.querySelectorAll(".screen-bottom [data-hero-shot]");
    var notes = document.querySelectorAll("[data-hero-notes]");
    var device = document.querySelector(".device-angled");
    if (tops.length < 2 || tops.length !== bottoms.length || reduceMotion) return;

    // The first set waits for the boot screen, so it stays up longest.
    var holds = [7000, 5500, 5500];
    var index = 0;

    function show(on) {
      [tops, bottoms, notes].forEach(function (list) {
        if (list[index]) list[index].classList.toggle("is-on", on);
      });
    }

    function next() {
      if (!document.hidden) {
        if (device) device.classList.add("is-cycling");
        show(false);
        index = (index + 1) % tops.length;
        show(true);
      }
      setTimeout(next, holds[index] || 5500);
    }
    setTimeout(next, holds[0]);
  }

  // The screens are drawn at native 3DS size and projected onto the photo by
  // fixed matrices computed for the photo at --photo-w pixels wide, so they
  // need scaling by (rendered width / --photo-w). CSS cannot turn a length
  // into a number, so the factor is set here and kept current as the stage
  // resizes.
  function setUpScreenScale() {
    document.querySelectorAll(".device-stage").forEach(function (stage) {
      var photoWidth = parseFloat(getComputedStyle(stage).getPropertyValue("--photo-w"));
      if (!photoWidth) return;
      function update() {
        var width = stage.offsetWidth;
        if (width > 0) stage.style.setProperty("--k", (width / photoWidth).toFixed(5));
      }
      update();
      if ("ResizeObserver" in window) {
        new ResizeObserver(update).observe(stage);
      } else {
        window.addEventListener("resize", update);
      }
      stage.classList.add("is-mapped");
    });
  }

  // A slight tilt of the device towards the pointer. Pointer only (no touch),
  // and off when reduced motion is requested.
  function setUpTilt() {
    var device = document.querySelector("[data-tilt]");
    var stage = device && device.querySelector(".device-stage");
    if (!device || !stage || reduceMotion) return;
    if (!window.matchMedia("(hover: hover) and (pointer: fine)").matches) return;

    var frame = null;
    function apply(x, y) {
      stage.style.transform = "rotateX(" + (y * -4).toFixed(2) + "deg) rotateY(" + (x * 6).toFixed(2) + "deg)";
    }
    device.addEventListener("pointermove", function (event) {
      var box = device.getBoundingClientRect();
      var x = (event.clientX - box.left) / box.width - 0.5;
      var y = (event.clientY - box.top) / box.height - 0.5;
      if (frame) cancelAnimationFrame(frame);
      frame = requestAnimationFrame(function () { apply(x, y); });
    });
    device.addEventListener("pointerleave", function () {
      if (frame) cancelAnimationFrame(frame);
      stage.style.transform = "";
    });
  }

  // The FAQ is the Notifications app: each question is marked new until it
  // is opened, and the browser remembers which ones were read.
  function setUpNotifications() {
    var faq = document.querySelector(".faq");
    var count = document.querySelector("[data-unread]");
    if (!faq) return;
    var rows = Array.prototype.slice.call(faq.querySelectorAll("details[id]"));
    var KEY = "3dsdl-read";
    var read = [];
    try {
      read = JSON.parse(localStorage.getItem(KEY) || "[]");
      if (!Array.isArray(read)) read = [];
    } catch (error) {
      read = [];
    }

    function update() {
      var unread = rows.filter(function (row) { return !row.classList.contains("is-read"); }).length;
      if (!count) return;
      count.textContent = unread ? unread + " new" : "All read";
      count.classList.toggle("is-done", !unread);
    }

    rows.forEach(function (row) {
      if (read.indexOf(row.id) !== -1) row.classList.add("is-read");
      row.addEventListener("toggle", function () {
        if (row.open) track("faq_opened", { question: row.id });
        if (!row.open || row.classList.contains("is-read")) return;
        row.classList.add("is-read");
        read.push(row.id);
        try {
          localStorage.setItem(KEY, JSON.stringify(read));
        } catch (error) {
          // Remembering is a nicety; the row still reads as read this visit.
        }
        update();
      });
      var summary = row.querySelector("summary");
      summary.addEventListener("click", function () {
        var opening = !row.open || row.classList.contains("is-closing");
        sfx.play(opening ? "open" : "close");
      });
    });
    update();
    // Rows already read appear read at once; only later changes animate.
    requestAnimationFrame(function () {
      requestAnimationFrame(function () { faq.classList.add("is-ready"); });
    });
  }

  // FAQ rows open and close with an eased height animation and a short fade
  // of the answer. Without the script, or with reduced motion, <details>
  // simply opens and closes on its own.
  function setUpFaq() {
    if (reduceMotion || !("animate" in Element.prototype)) return;
    var timing = { duration: 320, easing: "cubic-bezier(0.23, 1, 0.32, 1)" };

    document.querySelectorAll(".faq details").forEach(function (details) {
      var summary = details.querySelector("summary");
      var answer = summary && summary.nextElementSibling;
      if (!answer) return;
      var running = null;

      function settle(open) {
        details.open = open;
        details.classList.remove("is-closing");
        details.style.overflow = "";
        running = null;
      }

      function animate(from, to, opening) {
        if (running) running.cancel();
        details.style.overflow = "hidden";
        running = details.animate({ height: [from + "px", to + "px"] }, timing);
        answer.animate(
          opening
            ? { opacity: [0, 1], transform: ["translateY(-6px)", "none"] }
            : { opacity: [1, 0], transform: ["none", "translateY(-4px)"] },
          { duration: opening ? 280 : 200, easing: timing.easing, fill: "none" }
        );
        var current = running;
        // finished rejects when a later click cancels this animation; the
        // newer animation then owns the cleanup.
        running.finished.then(function () {
          if (running === current) settle(opening);
        }, function () {});
      }

      summary.addEventListener("click", function (event) {
        event.preventDefault();
        // Start from the height on screen, even mid-animation. Fractional
        // sizes avoid a 1px snap when the animation hands over.
        var height = function (element) { return element.getBoundingClientRect().height; };
        var style = getComputedStyle(details);
        var frame = parseFloat(style.borderTopWidth) + parseFloat(style.borderBottomWidth);
        var from = height(details);
        if (details.open && !details.classList.contains("is-closing")) {
          details.classList.add("is-closing");
          animate(from, height(summary) + frame, false);
        } else {
          details.classList.remove("is-closing");
          details.open = true;
          animate(from, height(summary) + height(answer) + frame, true);
        }
      });
    });
  }

  // The install tabs. Arrow keys, Home and End move between them (a roving
  // tabindex, as in the ARIA tabs pattern). Switching slides the old QR code
  // and details off both screens and the new ones on, in the tab's direction.
  function setUpInstaller() {
    var picker = document.querySelector(".picker");
    var panel = document.getElementById("install-screens");
    if (!picker || !panel) return;
    var picks = Array.prototype.slice.call(picker.querySelectorAll(".pick"));
    var animated = !reduceMotion && "animate" in Element.prototype;
    var easeOut = "cubic-bezier(0.23, 1, 0.32, 1)";
    var easeIn = "cubic-bezier(0.4, 0, 1, 1)";
    var views = Array.prototype.slice.call(panel.querySelectorAll(".view"));
    var turn = 0;

    function stopAll() {
      views.forEach(function (view) {
        view.getAnimations({ subtree: true }).forEach(function (animation) { animation.cancel(); });
      });
    }

    // On phones the touch screen is too small to read, so its views move out
    // below the console (and back when there is room again).
    var stepsBox = panel.querySelector(".install-steps");
    var bottomScreen = panel.querySelector(".screen-bottom");
    var infoViews = views.filter(function (view) { return view.classList.contains("info-view"); });
    var narrow = window.matchMedia("(max-width: 640px)");
    function placeSteps() {
      if (!stepsBox || !bottomScreen) return;
      var split = narrow.matches;
      panel.classList.toggle("is-split", split);
      infoViews.forEach(function (view) {
        (split ? stepsBox : bottomScreen).appendChild(view);
      });
    }
    placeSteps();
    if (narrow.addEventListener) {
      narrow.addEventListener("change", placeSteps);
    } else if (narrow.addListener) {
      narrow.addListener(placeSteps);
    }

    function showOnly(method) {
      views.forEach(function (view) { view.hidden = view.dataset.view !== method; });
    }

    function swap(method, direction) {
      var mine = ++turn;
      stopAll();
      var leaving = views.filter(function (view) {
        return !view.hidden && view.dataset.view !== method;
      });
      if (!animated || !leaving.length) {
        showOnly(method);
        return;
      }

      var exits = leaving.map(function (view, index) {
        return view.animate(
          { opacity: [1, 0], transform: ["none", "translateX(" + -direction * 24 + "px)"] },
          { duration: 160, delay: index * 30, easing: easeIn, fill: "forwards" }
        ).finished;
      });
      // A rejection means a newer click cancelled this swap and owns the views.
      Promise.all(exits).then(function () {
        if (mine !== turn) return;
        showOnly(method);
        stopAll();
        views.filter(function (view) { return !view.hidden; }).forEach(function (view, index) {
          var timing = { duration: 320, delay: index * 50, easing: easeOut, fill: "backwards" };
          view.animate(
            { opacity: [0, 1], transform: ["translateX(" + direction * 24 + "px)", "none"] },
            timing
          );
          var tile = view.querySelector(".qr-tile");
          if (tile) {
            tile.animate({ transform: ["scale(0.92)", "none"] }, { duration: 380, easing: easeOut, fill: "backwards" });
          }
        });
      }, function () {});
    }

    function select(pick, focus) {
      if (focus) pick.focus();
      if (pick.getAttribute("aria-selected") === "true") return;
      var before = picks.findIndex(function (other) {
        return other.getAttribute("aria-selected") === "true";
      });
      picks.forEach(function (other) {
        var on = other === pick;
        other.setAttribute("aria-selected", on ? "true" : "false");
        other.tabIndex = on ? 0 : -1;
      });
      picker.dataset.active = pick.dataset.method;
      panel.setAttribute("aria-labelledby", pick.id);
      sfx.play("swipe");
      track("install_method", { method: pick.dataset.method });
      swap(pick.dataset.method, picks.indexOf(pick) > before ? 1 : -1);
    }

    picks.forEach(function (pick) {
      pick.addEventListener("click", function () { select(pick, false); });
    });

    picker.addEventListener("keydown", function (event) {
      var index = picks.indexOf(document.activeElement);
      if (index < 0) return;
      var next = {
        ArrowLeft: (index - 1 + picks.length) % picks.length,
        ArrowRight: (index + 1) % picks.length,
        Home: 0,
        End: picks.length - 1
      }[event.key];
      if (next === undefined) return;
      event.preventDefault();
      select(picks[next], true);
    });
  }

  // The status bar clock, in the visitor's own time and format, with the
  // colon blinking like the 3DS's.
  function setUpClock() {
    var clock = document.querySelector("[data-clock]");
    if (!clock || !window.Intl) return;
    var hours = clock.querySelector("[data-hours]");
    var minutes = clock.querySelector("[data-minutes]");
    var format = new Intl.DateTimeFormat(undefined, { hour: "2-digit", minute: "2-digit" });
    function tick() {
      var parts = {};
      format.formatToParts(new Date()).forEach(function (part) { parts[part.type] = part.value; });
      hours.textContent = parts.hour || "--";
      minutes.textContent = (parts.minute || "--") + (parts.dayPeriod ? " " + parts.dayPeriod : "");
      setTimeout(tick, 60000 - (Date.now() % 60000) + 50);
    }
    tick();
  }

  // The features as a HOME Menu: a tab per icon (arrow keys move between
  // them, as in the ARIA tabs pattern), a selection frame that glides to the
  // selected icon, and the selected feature on the "top screen". While the
  // section is in view and nobody has touched it, it browses on its own.
  function setUpHome() {
    var home = document.querySelector("[data-home]");
    if (!home) return;
    var grid = home.querySelector(".home-grid");
    var cursor = home.querySelector(".home-cursor");
    var icons = Array.prototype.slice.call(home.querySelectorAll(".app-icon"));
    var panels = icons.map(function (icon) {
      return document.getElementById(icon.getAttribute("aria-controls"));
    });
    if (!grid || !cursor || !icons.length) return;
    var index = Math.max(0, icons.findIndex(function (icon) {
      return icon.getAttribute("aria-selected") === "true";
    }));

    // Layout positions (offsets ignore the selected icon's lift and scale).
    function place() {
      var icon = icons[index];
      var glyph = icon.querySelector(".app-glyph");
      if (!glyph || !icon.offsetWidth) return;
      var margin = 6;
      cursor.style.width = glyph.offsetWidth + margin * 2 + "px";
      cursor.style.height = glyph.offsetHeight + margin * 2 + "px";
      cursor.style.transform = "translate(" +
        (icon.offsetLeft + glyph.offsetLeft - margin) + "px, " +
        (icon.offsetTop + glyph.offsetTop - margin) + "px)";
    }

    function select(next, how) {
      if (next === index) {
        if (how === "key") icons[next].focus();
        return;
      }
      index = next;
      icons.forEach(function (icon, i) {
        var on = i === next;
        icon.setAttribute("aria-selected", on ? "true" : "false");
        icon.tabIndex = on ? 0 : -1;
        if (panels[i]) panels[i].classList.toggle("is-on", on);
      });
      place();
      if (how === "key") icons[next].focus();
      if (how !== "auto") {
        sfx.play("select");
        track("feature_viewed", { feature: icons[next].id.replace(/^tab-/, ""), how: how });
      }
    }

    place();
    requestAnimationFrame(function () { cursor.classList.add("is-ready"); });
    if ("ResizeObserver" in window) new ResizeObserver(place).observe(grid);
    if (document.fonts && document.fonts.ready) document.fonts.ready.then(place);

    icons.forEach(function (icon, i) {
      icon.addEventListener("click", function () { select(i, "click"); });
    });

    grid.addEventListener("keydown", function (event) {
      var columns = getComputedStyle(grid).gridTemplateColumns.split(" ").length || icons.length;
      var last = icons.length - 1;
      var next = {
        ArrowLeft: index === 0 ? last : index - 1,
        ArrowRight: index === last ? 0 : index + 1,
        ArrowUp: Math.max(0, index - columns),
        ArrowDown: Math.min(last, index + columns),
        Home: 0,
        End: last
      }[event.key];
      if (next === undefined) return;
      event.preventDefault();
      select(next, "key");
    });

    // Attract mode: off with reduced motion, paused while the pointer is over
    // the menu or the section is out of view, and over for good once someone
    // picks an icon themselves.
    if (reduceMotion || !("IntersectionObserver" in window)) return;
    var timer = null;
    var visible = false;
    var hovering = false;
    var done = false;
    function stop() {
      clearInterval(timer);
      timer = null;
    }
    function run() {
      stop();
      if (done || !visible || hovering) return;
      timer = setInterval(function () {
        if (!document.hidden) select((index + 1) % icons.length, "auto");
      }, 4200);
    }
    new IntersectionObserver(function (entries) {
      visible = entries[0].isIntersecting;
      run();
    }, { threshold: 0.5 }).observe(home);
    home.addEventListener("pointerenter", function () { hovering = true; run(); });
    home.addEventListener("pointerleave", function () { hovering = false; run(); });
    ["pointerdown", "keydown", "focusin"].forEach(function (type) {
      home.addEventListener(type, function () { done = true; stop(); });
    });
  }

  // The Reddit plaza. Every visit deals the posts out afresh: rows still
  // take quotes of similar length, so their bubbles stay close in height,
  // but the lengths are fuzzed so a quote can land in a neighbouring row,
  // and the order, the rows' bands and where each row starts are random.
  // A row holds its posts twice and drifts by one copy's width, so it wraps
  // without a seam. The drift is a Web Animation, which the compositor runs
  // without touching the DOM; the main thread only steps in to stop a row
  // under keyboard focus, and to follow a drag, swipe or sideways scroll,
  // after which the row waits a moment and eases back into its drift.
  // The pointer alone never stops a row.
  // With reduced motion the rows stay plain scrollers.
  function setUpPlaza() {
    var plaza = document.querySelector("[data-plaza]");
    if (!plaza) return;
    var strips = Array.prototype.slice.call(plaza.querySelectorAll(".plaza-track"));

    function shuffle(list) {
      for (var i = list.length - 1; i > 0; i--) {
        var j = Math.floor(Math.random() * (i + 1));
        var swap = list[i];
        list[i] = list[j];
        list[j] = swap;
      }
      return list;
    }

    var posts = [];
    strips.forEach(function (strip) { posts = posts.concat(Array.prototype.slice.call(strip.children)); });
    var dealt = posts.map(function (post) {
      return { post: post, key: post.querySelector(".post-text").textContent.length * (0.7 + Math.random() * 0.6) };
    }).sort(function (a, b) { return a.key - b.key; });
    // Bands from short to long; longer quotes drift slower, to stay readable.
    var bandSpeeds = [27, 22, 17];
    var bands = shuffle([0, 1, 2]);
    strips.forEach(function (strip, index) {
      var band = bands[index];
      var from = Math.round(dealt.length * band / 3);
      var to = Math.round(dealt.length * (band + 1) / 3);
      shuffle(dealt.slice(from, to)).forEach(function (item) { strip.appendChild(item.post); });
      strip.parentNode.dataset.speed = bandSpeeds[band];
    });

    plaza.addEventListener("click", function (event) {
      var post = event.target.closest(".post");
      if (post) track("reddit_comment_opened", { user: post.dataset.user });
    });

    // One tab stop per row, so the page's later sections stay a few tabs
    // away; the arrow keys, Home and End move along a row.
    strips.forEach(function (strip) {
      var list = Array.prototype.slice.call(strip.children);
      list.forEach(function (post, index) { post.tabIndex = index ? -1 : 0; });
      strip.addEventListener("keydown", function (event) {
        var index = list.indexOf(event.target);
        var next = { ArrowLeft: index - 1, ArrowRight: index + 1, Home: 0, End: list.length - 1 }[event.key];
        if (index < 0 || next === undefined) return;
        event.preventDefault();
        next = Math.max(0, Math.min(list.length - 1, next));
        list[index].tabIndex = -1;
        list[next].tabIndex = 0;
        list[next].focus();
      });
    });

    if (reduceMotion || !("IntersectionObserver" in window) || !plaza.animate) return;

    var rows = strips.map(function (strip) {
      var count = strip.children.length;
      for (var i = 0; i < count; i++) {
        var copy = strip.children[i].cloneNode(true);
        copy.setAttribute("aria-hidden", "true");
        copy.tabIndex = -1;
        strip.appendChild(copy);
      }
      var element = strip.parentNode;
      return {
        element: element,
        strip: strip,
        first: strip.children[0],
        copy: strip.children[count],
        direction: element.dataset.drift === "right" ? -1 : 1,
        speed: Number(element.dataset.speed),
        period: 0,
        animation: null,
        rate: 1,
        focus: false,
        press: null,
        resumeAt: 0
      };
    });
    plaza.classList.add("is-drifting");

    // x is how far the row has moved left, in [0, period).
    function mod(x, period) { return ((x % period) + period) % period; }
    function getX(row) {
      var duration = row.period / row.speed * 1000;
      var progress = mod(row.animation.currentTime || 0, duration) / duration;
      return row.direction > 0 ? progress * row.period : (1 - progress) * row.period;
    }
    function setX(row, x) {
      var duration = row.period / row.speed * 1000;
      var progress = mod(x, row.period) / row.period;
      row.animation.currentTime = (row.direction > 0 ? progress : mod(1 - progress, 1)) * duration;
    }

    // One copy's width, gap included, from fractional positions so the wrap
    // lands exactly. It changes when the fonts arrive or the page resizes.
    function measure(row) {
      var period = row.copy.getBoundingClientRect().left - row.first.getBoundingClientRect().left;
      if (!(period > 0) || Math.abs(period - row.period) < 0.01) return;
      var x = row.animation ? getX(row) / row.period : Math.random();
      row.period = period;
      var frames = [
        { transform: "translate3d(0, 0, 0)" },
        { transform: "translate3d(" + -period + "px, 0, 0)" }
      ];
      if (row.direction < 0) frames.reverse();
      var timing = { duration: period / row.speed * 1000, iterations: Infinity };
      if (row.animation) {
        row.animation.effect.setKeyframes(frames);
        row.animation.effect.updateTiming(timing);
      } else {
        row.animation = row.strip.animate(frames, timing);
        row.animation.playbackRate = row.rate;
        if (!visible) row.animation.pause();
      }
      setX(row, x * period);
    }

    var visible = false;
    rows.forEach(measure);
    if ("ResizeObserver" in window) {
      var resize = new ResizeObserver(function () { rows.forEach(measure); });
      rows.forEach(function (row) { resize.observe(row.strip); });
    }

    // Easing a row's speed to its target is the only per-frame work, and it
    // stops once every row has settled.
    var easing = 0;
    var last = 0;
    function target(row) {
      var held = row.focus || (row.press && row.press.moved) || performance.now() < row.resumeAt;
      return held ? 0 : 1;
    }
    function ease(now) {
      var seconds = Math.min((now - last) / 1000, 0.05);
      var busy = false;
      last = now;
      rows.forEach(function (row) {
        var goal = target(row);
        var rate = row.rate + (goal - row.rate) * Math.min(seconds * 6, 1);
        if (Math.abs(goal - rate) < 0.01) rate = goal;
        else busy = true;
        if (rate !== row.rate && row.animation) row.animation.playbackRate = row.rate = rate;
      });
      easing = busy ? requestAnimationFrame(ease) : 0;
    }
    function settle() {
      if (easing) return;
      last = performance.now();
      easing = requestAnimationFrame(ease);
    }
    function stop(row) {
      row.rate = 0;
      if (row.animation) row.animation.playbackRate = 0;
    }
    function holdThenDrift(row) {
      row.resumeAt = performance.now() + 1500;
      clearTimeout(row.resume);
      row.resume = setTimeout(settle, 1550);
    }

    // Run only near the screen. The avatars are lazy, but rows that clip
    // sideways hide them from the browser's lazy loading, so they all load
    // once the plaza is close.
    new IntersectionObserver(function (entries) {
      visible = entries[0].isIntersecting;
      if (visible) plaza.querySelectorAll("img[loading=lazy]").forEach(function (img) { img.loading = "eager"; });
      rows.forEach(function (row) {
        if (row.animation) row.animation[visible ? "play" : "pause"]();
      });
    }, { rootMargin: "200px 0px" }).observe(plaza);

    rows.forEach(function (row) {
      var element = row.element;
      var swallowClick = false;

      // Tabbing to a post stops the row and brings all of the post into
      // view, clear of the edge fades. A click focuses a post too, but that
      // shouldn't hold the row.
      element.addEventListener("focusin", function (event) {
        var post = event.target.closest(".post");
        if (!post || !post.matches(":focus-visible") || !row.animation) return;
        row.focus = true;
        stop(row);
        var edge = parseFloat(getComputedStyle(element).paddingLeft);
        var box = element.getBoundingClientRect();
        var rect = post.getBoundingClientRect();
        var shift = 0;
        if (rect.left - box.left < edge) shift = rect.left - box.left - edge;
        else if (box.right - rect.right < edge) shift = edge - (box.right - rect.right);
        if (shift) setX(row, Math.max(0, getX(row) + shift));
      });
      element.addEventListener("focusout", function (event) {
        if (element.contains(event.relatedTarget)) return;
        row.focus = false;
        settle();
      });

      // Dragging starts after a few pixels, so a tap or click still opens
      // the post, and the click that ends a drag is dropped.
      element.addEventListener("pointerdown", function (event) {
        if (event.button !== 0 || !row.animation) return;
        row.press = { id: event.pointerId, from: event.clientX, x: 0, moved: false };
      });
      element.addEventListener("pointermove", function (event) {
        var press = row.press;
        if (!press || press.id !== event.pointerId) return;
        var moved = event.clientX - press.from;
        if (!press.moved) {
          if (Math.abs(moved) < 6) return;
          press.moved = true;
          press.from = event.clientX;
          moved = 0;
          stop(row);
          press.x = getX(row);
          element.setPointerCapture(event.pointerId);
          element.classList.add("is-dragging");
        }
        setX(row, press.x - moved);
      });
      function release(event) {
        var press = row.press;
        if (!press || press.id !== event.pointerId) return;
        row.press = null;
        if (!press.moved) return;
        element.classList.remove("is-dragging");
        holdThenDrift(row);
        swallowClick = true;
        setTimeout(function () { swallowClick = false; }, 0);
      }
      element.addEventListener("pointerup", release);
      element.addEventListener("pointercancel", release);
      element.addEventListener("click", function (event) {
        if (!swallowClick) return;
        event.preventDefault();
        event.stopPropagation();
      }, true);
      element.addEventListener("dragstart", function (event) { event.preventDefault(); });

      // Sideways wheel and trackpad scrolling move the row; vertical scrolling
      // is left to the page, and the handler does no work for it.
      element.addEventListener("wheel", function (event) {
        if (Math.abs(event.deltaX) <= Math.abs(event.deltaY) || !row.animation) return;
        event.preventDefault();
        stop(row);
        setX(row, getX(row) + event.deltaX);
        holdThenDrift(row);
      }, { passive: false });
    });
  }

  // Scroll reveals. The reveal-ready class is added only here, so sections
  // are never hidden unless this code is running and can show them again.
  function setUpReveals() {
    var items = document.querySelectorAll(".reveal");
    if (!items.length || reduceMotion || !("IntersectionObserver" in window)) return;
    root.classList.add("reveal-ready");
    var observer = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (entry.isIntersecting) {
          entry.target.classList.add("is-in");
          observer.unobserve(entry.target);
        }
      });
    }, { rootMargin: "0px 0px -8% 0px", threshold: 0.08 });
    items.forEach(function (item) { observer.observe(item); });
  }

  // Highlight the tab for the section in view.
  function setUpTabs() {
    var tabs = Array.prototype.slice.call(document.querySelectorAll(".tabs a"));
    if (!tabs.length || !("IntersectionObserver" in window)) return;
    var byId = {};
    tabs.forEach(function (tab) { byId[tab.getAttribute("href").slice(1)] = tab; });
    var observer = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        var tab = byId[entry.target.id];
        if (!tab) return;
        if (entry.isIntersecting) {
          tabs.forEach(function (other) { other.classList.toggle("is-active", other === tab); });
        }
      });
    }, { rootMargin: "-40% 0px -50% 0px" });
    Object.keys(byId).forEach(function (id) {
      var section = document.getElementById(id);
      if (section) observer.observe(section);
    });
  }

  // Menu sounds, synthesised on the fly (no recordings of anyone's console).
  // On by default; browsers only allow audio after the first click or key
  // press, so they start from then.
  function setUpSounds() {
    var toggle = document.getElementById("sound-toggle");
    var AudioContextClass = window.AudioContext || window.webkitAudioContext;
    if (!toggle || !AudioContextClass) {
      if (toggle) toggle.hidden = true;
      return;
    }
    var context = null;
    var enabled = true;
    var lastHover = 0;

    try {
      enabled = localStorage.getItem("3dsdl-sounds") !== "off";
    } catch (error) {
      enabled = true;
    }

    function unlock() {
      if (!context) context = new AudioContextClass();
      if (context.state === "suspended") context.resume();
    }
    window.addEventListener("pointerdown", unlock, { passive: true });
    window.addEventListener("keydown", unlock);

    // One short note: a pitch glide with a quick attack and an exponential
    // fade, optionally delayed so notes can make a little tune.
    function tone(from, to, duration, volume, type, delay) {
      var start = context.currentTime + (delay || 0);
      var oscillator = context.createOscillator();
      var gain = context.createGain();
      oscillator.type = type || "sine";
      oscillator.frequency.setValueAtTime(from, start);
      oscillator.frequency.exponentialRampToValueAtTime(to, start + duration);
      gain.gain.setValueAtTime(0.0001, start);
      gain.gain.linearRampToValueAtTime(volume, start + 0.006);
      gain.gain.exponentialRampToValueAtTime(0.0001, start + duration);
      oscillator.connect(gain).connect(context.destination);
      oscillator.start(start);
      oscillator.stop(start + duration + 0.02);
    }

    var sounds = {
      hover: function () { tone(1800, 1500, 0.03, 0.03); },
      click: function () { tone(520, 880, 0.09, 0.06); },
      select: function () {
        tone(1175, 1175, 0.05, 0.045, "triangle");
        tone(1568, 1568, 0.07, 0.04, "triangle", 0.05);
      },
      swipe: function () { tone(420, 980, 0.12, 0.05); },
      open: function () { tone(660, 1100, 0.1, 0.055); },
      close: function () { tone(900, 520, 0.1, 0.05); },
      copy: function () {
        tone(784, 784, 0.08, 0.045, "triangle");
        tone(988, 988, 0.08, 0.045, "triangle", 0.06);
        tone(1319, 1319, 0.14, 0.045, "triangle", 0.12);
      },
      message: function () {
        tone(1319, 1319, 0.07, 0.05, "triangle");
        tone(1760, 1760, 0.12, 0.05, "triangle", 0.08);
      },
      toggle: function () { tone(660, 990, 0.08, 0.06); }
    };

    sfx.play = function (name) {
      if (!enabled || !context || context.state !== "running") return;
      (sounds[name] || sounds.click)();
    };

    function render() {
      toggle.setAttribute("aria-pressed", enabled ? "true" : "false");
      toggle.querySelector("use").setAttribute("href", enabled ? "#i-sound" : "#i-mute");
      toggle.querySelector("span").textContent = enabled ? "Menu sounds on" : "Menu sounds off";
    }
    render();

    toggle.addEventListener("click", function () {
      enabled = !enabled;
      try {
        localStorage.setItem("3dsdl-sounds", enabled ? "on" : "off");
      } catch (error) {
        // Remembering the choice is a nicety; it still applies to this visit.
      }
      render();
      sfx.play("toggle");
      track("menu_sounds", { on: enabled });
    });

    document.addEventListener("pointerover", function (event) {
      var target = event.target.closest("[data-sound], .btn, .tabs a, .gh-link, .fund, .app-icon, .pick, .faq summary, .sbtn");
      if (!target || target.contains(event.relatedTarget)) return;
      var now = Date.now();
      if (now - lastHover < 70) return;
      lastHover = now;
      sfx.play("hover");
    });
    // Buttons and tabs click; the menu, installer and FAQ play their own.
    document.addEventListener("click", function (event) {
      var target = event.target.closest("[data-sound], .btn, .tabs a, .gh-link, .fund, .sbtn, .brand");
      if (target) sfx.play(target.getAttribute("data-sound") || "click");
    });
  }

  // The video's poster is a link to YouTube. A plain click swaps in the
  // youtube-nocookie player instead and starts it, so nothing loads from
  // YouTube before then; a middle or modified click still opens YouTube.
  function setUpVideo() {
    var tv = document.querySelector("[data-video]");
    var poster = tv && tv.querySelector(".tv-poster");
    if (!poster) return;
    poster.addEventListener("click", function (event) {
      if (event.button !== 0 || event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return;
      event.preventDefault();
      var frame = document.createElement("iframe");
      frame.src = "https://www.youtube-nocookie.com/embed/" + tv.dataset.video + "?autoplay=1&rel=0&playsinline=1";
      frame.title = "3dsDL video";
      frame.allow = "autoplay; encrypted-media; picture-in-picture; fullscreen; web-share";
      frame.allowFullscreen = true;
      frame.referrerPolicy = "strict-origin-when-cross-origin";
      tv.replaceChild(frame, poster);
      frame.focus();
    });
  }

  showRelease();
  showDownloads();
  setUpVideo();
  setUpCopyButtons();
  setUpSendTool();
  setUpScreenScale();
  setUpDeviceDemo();
  setUpTilt();
  setUpClock();
  setUpHome();
  setUpNotifications();
  setUpFaq();
  setUpPlaza();
  setUpInstaller();
  setUpReveals();
  setUpTabs();
  setUpSounds();
})();

// The buddy card on the right edge: the sibling app, peeking in. Hover
// opens it on a pointer; a tap on its tab opens and closes it on touch.
(function () {
  "use strict";
  var buddy = document.getElementById("buddy");
  if (!buddy) return;
  var tab = buddy.querySelector(".buddy-tab");
  tab.addEventListener("click", function (event) {
    event.preventDefault();
    if (buddy.classList.toggle("is-open")) window.track("buddy_opened", { app: "3dsFM" });
  });
  if (location.search.indexOf("buddy=open") !== -1) buddy.classList.add("is-open");
})();

// Support on the left edge. Wide screens show it outright; narrower ones
// open it with its tab, which a tap also closes.
(function () {
  "use strict";
  var funds = document.getElementById("funds");
  if (!funds) return;
  var tab = funds.querySelector(".funds-tab");
  tab.addEventListener("click", function () {
    tab.setAttribute("aria-expanded", funds.classList.toggle("is-open") ? "true" : "false");
  });
})();
