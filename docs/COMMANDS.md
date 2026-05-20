# Команды проверки

## Проверка старого режима изображения

```bat
diplom.exe source.png -m ycbcr -b 8 -q 16 -o output_decoded.png -w output_encoded.walsh
diplom.exe output_encoded.walsh -o output_decoded.png
diplom.exe output_encoded.walsh -info
diplom.exe -diff source.png output_decoded.png diff.png 16
```

## Полный цикл видео

```bat
diplom.exe source.mp4 -fps 1 -m ycbcr -b 8 -q 16 -w output_encoded.wlsv
diplom.exe output_encoded.wlsv -o output_decoded.mp4
```

## Сравнение параметров квантования

```bat
diplom.exe source.mp4 -fps 1 -m ycbcr -b 8 -q 8  -w output_encoded_q8.wlsv
diplom.exe source.mp4 -fps 1 -m ycbcr -b 8 -q 16 -w output_encoded_q16.wlsv
diplom.exe source.mp4 -fps 1 -m ycbcr -b 8 -q 32 -w output_encoded_q32.wlsv
```

## RGB и grayscale

```bat
diplom.exe source.mp4 -fps 1 -m rgb  -b 8 -q 16 -w output_encoded_rgb_q16.wlsv
diplom.exe source.mp4 -fps 1 -m gray -b 8 -q 16 -w output_encoded_gray_q16.wlsv
```
