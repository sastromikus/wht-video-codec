# Walsh Video Codec

Учебный C++ прототип intra-only кодека полноэкранного кадра на основе преобразования Уолша-Адамара.

Проект не является промышленным аналогом H.264, H.265 или AV1. Основная цель — показать полный цикл трансформного сжатия кадра: загрузка изображения или видеокадра, блочное WHT-преобразование, квантование, упаковка коэффициентов, сохранение во внутренний формат и обратное восстановление.

## Возможности

- загрузка изображений PNG, BMP, JPG, TGA;
- режимы `grayscale`, `RGB`, `YCbCr`;
- блочное WHT-преобразование для блоков `2x2`, `4x4`, `8x8`;
- квантование коэффициентов;
- zig-zag обход для блока `8x8`;
- отсечение малых высокочастотных коэффициентов;
- DC-delta для первого коэффициента блока;
- RLE и компактные токены;
- zlib-сжатие payload;
- собственный формат одного кадра `.walsh`;
- собственный intra-only видеоформат `.wlsv`;
- чтение видео через FFmpeg libraries;
- декодирование `.wlsv` обратно в MP4;
- расчёт MSE и PSNR;
- режимы `-info`, `-dump`, `-diff`, `-test`, `-test-codecs`.

## Ограничения

- каждый кадр кодируется независимо;
- межкадровое предсказание, компенсация движения, GOP и управление битрейтом не реализованы;
- `.wlsv` является исследовательским intra-only контейнером;
- альфа-канал PNG не кодируется, изображение приводится к RGB;
- FFmpeg используется только для чтения/записи распространённых видеоформатов, а не для Walsh-кодирования.

## Зависимости

Рекомендуемый способ установки зависимостей под Windows и Visual Studio — `vcpkg`:

```bat
cd C:\
git clone https://github.com/microsoft/vcpkg.git
cd C:\vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg integrate install
.\vcpkg install ffmpeg:x64-windows zlib:x64-windows
```

Для запуска собранного `diplom.exe` вне Visual Studio может понадобиться положить рядом DLL из:

```text
C:\vcpkg\installed\x64-windows\bin
```

## Сборка через Visual Studio

1. Установить зависимости через `vcpkg`.
2. Открыть проект в Visual Studio.
3. Выбрать платформу `x64`.
4. Собрать `Release` или `Debug`.

## Сборка через CMake

```bat
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release
```

Исполняемый файл будет находиться в каталоге сборки, например:

```text
build\Release\diplom.exe
```

## Основные команды

Кодирование изображения в `.walsh`:

```bat
diplom.exe source.png -m ycbcr -b 8 -q 16 -o output_decoded.png -w output_encoded.walsh
```

Декодирование `.walsh`:

```bat
diplom.exe output_encoded.walsh -o output_decoded.png
```

Информация о `.walsh`:

```bat
diplom.exe output_encoded.walsh -info
```

Карта различий:

```bat
diplom.exe -diff source.png output_decoded.png diff.png 16
```

Кодирование видео в `.wlsv` с выборкой 1 fps:

```bat
diplom.exe source.mp4 -fps 1 -m ycbcr -b 8 -q 16 -w output_encoded.wlsv
```

Декодирование `.wlsv` обратно в MP4:

```bat
diplom.exe output_encoded.wlsv -o output_decoded.mp4
```

## Структура проекта

```text
main.cpp                 CLI и запуск режимов программы
codec.cpp/.h             WHT-кодирование и декодирование одного кадра
wht.cpp/.h               быстрое преобразование Уолша-Адамара
image_io.cpp/.h          загрузка/сохранение изображений и цветовые преобразования
encoded_io.cpp/.h        формат одного кадра .walsh
encoded_video_io.cpp/.h  формат последовательности кадров .wlsv
video_io_ffmpeg.cpp/.h   чтение/запись видео через FFmpeg libraries
metrics.cpp/.h           MSE и PSNR
test_runner.cpp/.h       автоматические тестовые сценарии
stb_image*.h             сторонние single-header библиотеки для изображений
```

## Форматы

`.walsh` хранит один независимо закодированный кадр.

`.wlsv` хранит последовательность независимо закодированных Walsh-кадров. Это intra-only видеоформат без межкадрового предсказания.

## Лицензия

Лицензия проекта пока не указана. Перед публичной публикацией на GitHub желательно добавить `LICENSE`, например MIT, Apache-2.0 или другой вариант по выбору автора.
