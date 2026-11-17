@echo off
rem Compile l'APK debug avec la toolchain installee sur ce PC (voir android/README.md).
setlocal
set "JAVA_HOME=%LOCALAPPDATA%\android-toolchain\jdk-17"
set "ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk"
set "ANDROID_SDK_ROOT=%ANDROID_HOME%"
set "GRADLE_USER_HOME=%LOCALAPPDATA%\android-toolchain\gradle-home"
set "GRADLE_BIN=%LOCALAPPDATA%\android-toolchain\gradle-8.7\bin\gradle.bat"

if not exist "%JAVA_HOME%\bin\java.exe" (echo ERREUR : JDK 17 absent de %JAVA_HOME% & exit /b 1)
if not exist "%GRADLE_BIN%" (echo ERREUR : Gradle 8.7 absent de %GRADLE_BIN% & exit /b 1)
if not exist "%ANDROID_HOME%\platforms\android-34" (echo ERREUR : platform android-34 absente & exit /b 1)

call "%GRADLE_BIN%" -p "%~dp0." --console=plain assembleDebug
if errorlevel 1 (echo ECHEC DE COMPILATION & exit /b 1)

echo.
echo APK genere : %~dp0app\build\outputs\apk\debug\app-debug.apk
endlocal
