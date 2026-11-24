# Konsoll site

The project's home page, built by Jekyll and published to GitHub Pages
(https://konsoll.app, a custom domain on rask-project.github.io) by `.github/workflows/pages.yml`
on every push to `main` that touches it.

- `index.html` (home), `features.html`, `agents.html`, `download.html`,
  `news/index.html` (the posts) and `404.html`.
- `_layouts/` (`default` → `page`, `post`) and `_includes/` (menu bar,
  footer, `icon.html` for a line icon, `shot.html` for a screenshot).
- `assets/css/site.css` (all styles; dark, macOS look) and
  `assets/js/site.js` (the Download buttons point at the latest release's
  `.dmg` through the GitHub API; Copy buttons; the Features sidebar).
- Screenshots, icons and the app icon are **not** kept here:
  `tools/site-assets.sh` copies `docs/images`, `resources/icons/konsoll/svg`
  and the app icon (`docs/images/app-icon.png`) into `assets/` (ignored by git)
  before a build.

## Preview

```sh
cd site && bundle install   # once: the github-pages gem (Ruby 3.x)
tools/site-serve.sh         # http://127.0.0.1:4000/
```

## A new post

Add `_posts/YYYY-MM-DD-slug.md`:

```markdown
---
title: "Konsoll 3.8.0: …"
summary: "One sentence for the News list and the page's lead."
release: v3.8.0   # optional: links the GitHub release
---

Markdown body (a release's notes from docs/releases/ fit as they are).
```
