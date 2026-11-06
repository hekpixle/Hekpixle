# Hekpixle — نسخه‌ی C++ (ساختار تخت)

بک‌اند C++ (Win32) + رابط WebView2؛ رابط (`index.html`, `app.js`, `styles.css`) همان نسخه‌ی Electron است.

## چرا همه‌چیز بیرون است؟
GitHub (آپلود از وب) پوشه نمی‌سازد؛ پس همه‌ی فایل‌ها کنار هم‌اند. **CMake بعد از بیلد خودش** فایل‌ها را در ساختار پوشه‌ای که برنامه لازم دارد کنار exe می‌چیند:
```
build/out/Hekpixle.exe  WebView2Loader.dll
build/out/renderer/ (index.html, app.js, styles.css, fonts/, lib/katex/..., lib/highlight/...)
build/out/assets/   build/out/skills_seed/<name>/SKILL.md
```

## فایل‌ها
- کد: `main.cpp window.cpp ipc.cpp ipc_shim.cpp agent.cpp context.cpp util.cpp` + ابزارها `web files computer browser mcp media procs ps skills cron plugins agent_tools telegram (.cpp)` + هدرها `*.hpp`
- `nlohmann_json.hpp` (کتابخانه‌ی JSON، تک‌هدر)
- `Hekpixle.rc.in`, `Hekpixle.manifest`, `icon.*` (آیکون/مانیفست)
- رابط: `index.html app.js styles.css marked.min.js katex.min.* auto-render.min.js highlight.min.js github*.css` + فونت‌ها (`Vazirmatn-*`, `SpaceGrotesk-*`, `KaTeX_*`)
- `skill-*.md` ← skillهای اولیه (به `skills_seed/<name>/SKILL.md` کپی می‌شوند)
- `CMakeLists.txt`, `build.bat`, `github-workflow-build.yml`

## ساخت exe
**GitHub Actions (بدون نصب):** فایل `github-workflow-build.yml` را با Add file ← Create new file به نام `.github/workflows/build.yml` بسازید (محتوا را کپی کنید) ← تب Actions ← Build Hekpixle (C++) ← Run workflow ← `Hekpixle-win64` از Artifacts.
**روی ویندوز:** Visual Studio Build Tools (Desktop development with C++) + CMake، سپس `build.bat`. بار اول اینترنت لازم است (WebView2 SDK از nuget).

داده‌ها: `%APPDATA%\Hekpixle\` — پروفایل جدا: `Hekpixle.exe --profile=work`.
