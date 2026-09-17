# eratostenes

Criba de Eratostenes segmentada, paralela y empaquetada en bits, pensada para
generar listados de primos muy grandes (probado hasta 10^11 = 100 000
millones) en un tiempo razonable.

## Resultado de referencia

```
$ ./eratostenes --limit 100b -o primos_100b.txt -t 12
Listo. 4118054813 primos encontrados hasta 100000000000 (48.90 GB).
  conteo:     23.52s
  escritura:  238.32s
  total:      261.84s (15.7 millones de primos/seg)
```

Medido en un Intel Core i5-11400F (12 hilos logicos), sobre WSL2/ext4, disco
con ~300 MB/s de escritura secuencial sostenida. El resultado (4 118 054 813
primos) coincide exactamente con el valor conocido de pi(10^11).

## Compilar

Requiere un compilador con C++20 y `pwrite`/`ftruncate` de POSIX (Linux o
WSL; no compila tal cual con MSVC/Windows nativo).

```
make            # build de produccion: -O3 -march=native -flto
make portable   # sin -march=native, para llevar el binario a otra maquina
make debug      # con ASan/UBSan, para depurar
```

Si trabajas en Windows con el proyecto en `/mnt/c/...`, compila y ejecuta
**dentro de WSL** apuntando la salida a un directorio nativo de Linux (p.ej.
`~/...`), no a `/mnt/c/...`: ese punto de montaje pasa por 9p y es mucho mas
lento para E/S intensiva. En las pruebas, escribir en `/mnt/c` fue el cuello
de botella real, no la CPU.

## Docker

```
make docker                                              # construye la imagen
make run ARGS="--limit 100b -o /output/primos.txt -t 12" # ejecuta desde la imagen
```

`make run` monta `./output` (host) en `/output` (contenedor) y crea el
directorio si no existe; usa siempre `-o /output/<fichero>` en `ARGS` para
que el resultado quede accesible fuera del contenedor. Sin `ARGS`, se
ejecuta con `--help`.

## Uso

```
./eratostenes --limit N [opciones]

  -n, --limit N          Limite superior (inclusive). Acepta sufijos:
                          k=1e3  m=1e6  b=1e9 (billon ingles)  t=1e12
                          Ej: 100b = 10^11
  -o, --output PATH      Fichero de salida (default: primes.txt)
  -t, --threads N        Numero de hilos (default: nucleos disponibles)
  -s, --segment-width N  Ancho numerico de cada segmento (default: 262144)
  -h, --help             Ayuda
```

Ejemplos:

```
./eratostenes -n 1000000 -o primos_1M.txt
./eratostenes -n 100b -o ~/primos_100b.txt -t 12
```

## Diseno

**Bits, no bytes.** Cada segmento se representa como un array de bits
(`uint64_t` words) donde cada bit corresponde a un numero impar del rango
(los pares se descartan de entrada salvo el 2, que es un caso especial). Un
bit a 1 significa "compuesto"; los primos se extraen invirtiendo la palabra
y recorriendo los bits puestos a 1 con `__builtin_ctzll` + "clear lowest set
bit", en vez de comprobar bit a bit.

**Criba fragmentada (segmentada).** Se calculan primero los primos base
(<= sqrt(N)) con una criba simple en memoria (siempre pequena: sqrt(10^11)
~= 316 228). Despues, el rango [3, N] se recorre en segmentos pequenos
(por defecto ~256K numeros, configurable con `-s`) que caben en cache L1/L2,
marcando multiplos de cada primo base dentro de cada segmento.

**Paralelizacion.** El rango completo se divide en tantos fragmentos
contiguos como hilos, cada uno cribando su propio tramo de forma
independiente (los primos base son de solo lectura, compartidos sin
bloqueos). El trabajo por numero es aproximadamente uniforme, asi que un
reparto estatico en fragmentos iguales ya da buen balance de carga.

**Escritura sin fusion posterior.** La primera version de este proyecto
escribia un fichero temporal por hilo y despues los concatenaba. Eso
duplica la E/S en disco: cada byte del resultado se escribe una vez al
fichero temporal y otra vez al fusionar. Con 45-49 GB de salida, esa segunda
copia fue el cuello de botella (57s de 64s en una prueba con 10^10).

En su lugar, el programa hace dos pasadas:

1. **Conteo** (`ByteCounter`): cada hilo criba su fragmento igual que en la
   pasada final, pero en vez de escribir, solo cuenta cuantos bytes de texto
   ocuparan sus primos. No hay E/S en esta fase.
2. Con esos totales se calcula por prefijos el offset exacto donde debe
   empezar a escribir cada hilo, y se redimensiona el fichero final a su
   tamano definitivo (`ftruncate`/`resize_file`, operacion barata incluso
   para ficheros de decenas de GB).
3. **Escritura** (`DirectWriter`): cada hilo vuelve a cribar su fragmento y
   escribe con `pwrite()` directamente en su region (disjunta) del fichero
   final, en paralelo con el resto.

El coste que se paga es repetir la fase de marcado de bits dos veces (barata,
limitada por CPU/cache: ~24s de 262s en la prueba de 10^11). A cambio se
evita repetir la escritura a disco (cara, limitada por I/O), que es el
recurso mas escaso a esta escala. En la practica esto dio un x2.9 de mejora
frente al esquema con fusion (22s vs 64s cribando hasta 10^10).

## Limitaciones y posibles mejoras futuras

- Solo se descartan los pares (ademas del 2). Una rueda modulo 30 (descartar
  tambien multiplos de 3 y 5) reduciria el trabajo de marcado y la memoria
  por segmento en, aproximadamente, otro factor 1.3-1.5x. No se implemento
  para mantener el codigo simple y facil de verificar; el cuello de botella
  actual a gran escala es la E/S de disco, no el marcado de bits.
- El offset de escritura de cada hilo se basa en `pwrite`, especifico de
  POSIX. Para un build nativo de Windows habria que sustituirlo por
  `WriteFile` con `OVERLAPPED` (offset explicito) o volver a un esquema de
  ficheros temporales + fusion.
- No se comprueba espacio libre en disco antes de empezar: para N=100b hacen
  falta unos 49 GB libres en el destino.

## Verificacion

Se comprobo que el conteo de primos coincide con pi(N) conocido para
N = 10, 100, 1000, 10^6, 2*10^6, 10^9, 10^10 y 10^11, y que el resultado es
identico (mismo hash) al variar el numero de hilos (1, 7, 12), lo que
descarta errores en los limites entre fragmentos.
