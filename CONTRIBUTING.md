# Contributing to sticky-claude

Thanks for taking a look. This is a small hobby/open-source project, not a
corporate one, so the bar is "be kind and explain your reasoning," not a
formal process.

## Ways to contribute

- **Bug reports**: open an issue. For firmware bugs, include the serial
  log around the failure if you can grab it (`idf.py monitor`).
- **Feature ideas**: open an issue first for anything nontrivial (new
  calendar provider, UI redesign, etc.) so we can talk through the
  approach before you sink time into it.
- **New plugins**: giving Claude a new capability (todo lists, smart
  home, whatever) almost certainly means writing a plugin rather than
  touching `app.py` -- see `backend/plugins/base.py` for the contract and
  `backend/plugins/calendar_plugin.py` for a worked example. Still worth
  an issue first if it's a bigger integration.
- **Docs fixes**: just open a PR, no need to ask first.
- **Code**: see below.

## Development setup

### Firmware

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html)
v5.3.x.

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

See `firmware/README.md` for the pin map, architecture, and known gaps.

### Backend

Requires Python 3.11+.

```bash
cd backend
cp .env.example .env   # fill in ANTHROPIC_API_KEY
pip install -r requirements.txt
uvicorn app:app --reload --port 8000
```

See `backend/README.md` for calendar setup and the dashboard.

## Before opening a PR

- **Firmware**: run `idf.py build` and make sure it's clean. If you can
  test on real hardware, say so in the PR description (and what you
  tested) -- a lot of this project's bugs have been things that only show
  up with a real panel/mic/button in hand, not in compilation.
- **Backend**: make sure `python -m py_compile` passes on changed files,
  and if you touched `/converse` or the calendar tool-use loop, describe
  what you tested it against (even a fake-credentials smoke test is
  useful context for reviewers).
- Keep commits focused. A PR that fixes one thing is easier to review
  than one that fixes a bug and reorganizes three files along the way.
- If you're porting or adapting code from another project (the way this
  repo's ePaper driver was ported from `folloup-sticky`), say so and note
  the license implications -- see the "Licensing" section below.

## Licensing

This repository is MIT-licensed, **except**
`firmware/components/epaper_display/`, which is GPLv3 because its SSD1677
command sequence is ported from the GPLv3-licensed
[`folloup-sticky`](https://github.com/alxv2016/folloup-sticky) project. If
you contribute code derived from another GPL project, the derived file(s)
need to carry that license too -- don't silently relicense someone else's
work. If you're not sure whether something counts as "derived," ask in
the PR.

## Code style

- **C++ (firmware)**: match the existing style in the file you're editing
  (Google-ish: `PascalCase` for functions/types, `snake_case_` for private
  members, 4-space indent). No exceptions/RTTI (the build disables both).
- **Python (backend)**: standard-ish PEP 8, type hints where they add
  clarity, no enforced formatter yet -- just be consistent with
  surrounding code.
- Comments explain *why*, not *what*. If a comment just restates the code
  in English, delete it.

## Questions

Open an issue, or start a discussion if the repo has Discussions enabled.
There's no mailing list, Discord, or other channel for this project.
