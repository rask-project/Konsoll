// Konsoll site: download link, copy buttons and the Features sidebar.

(function () {
    "use strict";

    // Point every Download button at the latest release's .dmg and show its
    // version. Without the API (offline, rate limit) they keep linking to the
    // Releases page.
    function resolveDownload() {
        var links = document.querySelectorAll("[data-download]");
        var versions = document.querySelectorAll("[data-version]");
        if (!links.length && !versions.length) {
            return;
        }
        fetch("https://api.github.com/repos/rask-project/Konsoll/releases/latest", {
            headers: { Accept: "application/vnd.github+json" }
        })
            .then(function (response) {
                return response.ok ? response.json() : null;
            })
            .then(function (release) {
                if (!release) {
                    return;
                }
                var dmg = (release.assets || []).find(function (asset) {
                    return /\.dmg$/i.test(asset.name);
                });
                var version = String(release.tag_name || "").replace(/^v/, "");
                links.forEach(function (link) {
                    if (dmg) {
                        link.href = dmg.browser_download_url;
                    }
                });
                versions.forEach(function (node) {
                    if (version) {
                        node.textContent = node.getAttribute("data-version").replace("%v", version);
                        node.hidden = false;
                    }
                });
            })
            .catch(function () {});
    }

    // A Copy button on every code block marked .code-copy.
    function addCopyButtons() {
        document.querySelectorAll(".code-copy").forEach(function (block) {
            var code = block.querySelector("code");
            if (!code || !navigator.clipboard) {
                return;
            }
            var button = document.createElement("button");
            button.type = "button";
            button.textContent = "Copy";
            button.addEventListener("click", function () {
                navigator.clipboard.writeText(code.textContent.trim()).then(function () {
                    button.textContent = "Copied";
                    setTimeout(function () {
                        button.textContent = "Copy";
                    }, 1600);
                });
            });
            block.appendChild(button);
        });
    }

    // Highlight the sidebar item of the section in view, like System Settings.
    function trackSettingsSidebar() {
        var links = Array.prototype.slice.call(document.querySelectorAll(".settings-sidebar a[href^='#']"));
        if (!links.length || !("IntersectionObserver" in window)) {
            return;
        }
        var byId = {};
        links.forEach(function (link) {
            byId[link.getAttribute("href").slice(1)] = link;
        });
        var observer = new IntersectionObserver(function (entries) {
            entries.forEach(function (entry) {
                if (!entry.isIntersecting) {
                    return;
                }
                links.forEach(function (link) {
                    link.classList.remove("is-current");
                });
                var current = byId[entry.target.id];
                if (current) {
                    current.classList.add("is-current");
                    // The sidebar is a horizontal strip on narrow screens:
                    // keep the item in view without scrolling the page.
                    var list = current.closest("ul");
                    var offset = current.getBoundingClientRect().left - list.getBoundingClientRect().left;
                    list.scrollLeft += offset - (list.clientWidth - current.offsetWidth) / 2;
                }
            });
        }, { rootMargin: "-30% 0px -60% 0px" });
        Object.keys(byId).forEach(function (id) {
            var section = document.getElementById(id);
            if (section) {
                observer.observe(section);
            }
        });
    }

    document.addEventListener("DOMContentLoaded", function () {
        resolveDownload();
        addCopyButtons();
        trackSettingsSidebar();
    });
})();
