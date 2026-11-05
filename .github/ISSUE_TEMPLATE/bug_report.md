---
name: Bug report
about: Something isn't working
title: ""
labels: bug
---

**Where's the bug?**
- [ ] Firmware (device)
- [ ] Backend (Pi/ZimaOS service)
- [ ] Dashboard
- [ ] Something else: ___

**What happened?**
A clear description of the actual behavior.

**What did you expect to happen?**

**Steps to reproduce**
1.
2.
3.

**Logs**
Firmware: paste the relevant `idf.py monitor` output around the failure.
Backend: paste the relevant lines from the backend's console/log output.

**Environment**
- Firmware: ESP-IDF version, and whether you changed anything from the
  default `sdkconfig.defaults`
- Backend: OS/hardware it's running on (Pi model, ZimaOS, etc.), Python
  version
- Calendar provider in use, if relevant: CalDAV / Google / none
