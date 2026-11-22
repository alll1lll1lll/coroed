#!/usr/bin/env python3
"""Построить графики и сводную таблицу по bench/results/results.csv.

Запуск из корня репозитория: python3 bench/plot.py
По каждой точке (сервер, число соединений) берется медиана повторов.
"""

import csv
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import FuncFormatter  # noqa: E402

RESULTS = Path(__file__).parent / "results"

SERIES = {
    # имя в CSV: (подпись, цвет, маркер)
    "coroed": ("Coroed (корутины + epoll)", "#2a78d6", "o"),
    "threads": ("Потоки ОС", "#eb6834", "s"),
}
METRICS = ["rps", "p50_ms", "p90_ms", "p99_ms", "errors", "rss_mb", "threads"]

INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
GRID = "#e4e3df"


def load():
    runs = defaultdict(lambda: defaultdict(list))
    with open(RESULTS / "results.csv", newline="") as f:
        for row in csv.DictReader(f):
            key = (row["server"], int(row["connections"]))
            for m in METRICS:
                runs[key][m].append(float(row[m]))
    return {key: {m: statistics.median(v) for m, v in metrics.items()} for key, metrics in runs.items()}


def style(ax, title, ylabel):
    ax.set_title(title, loc="left", fontsize=12, color=INK, pad=10)
    ax.set_xlabel("Одновременных соединений", color=INK_SECONDARY)
    ax.set_ylabel(ylabel, color=INK_SECONDARY)
    ax.set_xscale("log")
    ax.grid(True, which="major", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=INK_SECONDARY)


def plot(data, metric, title, ylabel, filename, scale=1.0, fmt="{:.0f}"):
    fig, ax = plt.subplots(figsize=(7, 4), dpi=200)
    connections = sorted({c for _, c in data})
    last = {}
    for server, (label, color, marker) in SERIES.items():
        xs = [c for c in connections if (server, c) in data]
        ys = [data[(server, c)][metric] * scale for c in xs]
        ax.plot(xs, ys, color=color, linewidth=2, marker=marker, markersize=6, label=label)
        last[server] = (xs[-1], ys[-1])
    # Подписи значений у последних точек; если точки близко,
    # большее значение подписываем выше, меньшее -- ниже.
    ordered = sorted(last.items(), key=lambda item: item[1][1])
    for rank, (server, (x, y)) in enumerate(ordered):
        dy = 0
        if len(ordered) == 2 and abs(ordered[1][1][1] - ordered[0][1][1]) < 0.06 * ax.get_ylim()[1]:
            dy = 7 if rank == 1 else -7
        ax.annotate(fmt.format(y), (x, y), textcoords="offset points",
                    xytext=(7, dy), va="center", fontsize=8, color=INK)
    style(ax, title, ylabel)
    ax.set_xticks(connections)
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{int(v)}"))
    ax.set_ylim(bottom=0)
    ax.legend(frameon=False, loc="best")
    fig.tight_layout()
    fig.savefig(RESULTS / filename)
    plt.close(fig)


def main():
    data = load()
    plot(data, "rps", "Пропускная способность", "Тыс. запросов в секунду", "rps.png",
         scale=1 / 1000, fmt="{:.0f}")
    plot(data, "p99_ms", "Задержка, 99-й перцентиль", "мс", "p99.png", fmt="{:.1f}")
    plot(data, "rss_mb", "Память сервера (RSS)", "МБ", "memory.png", fmt="{:.0f}")

    with open(RESULTS / "summary.csv", "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["server", "connections"] + METRICS)
        for (server, c) in sorted(data, key=lambda k: (k[0], k[1])):
            writer.writerow([server, c] + [data[(server, c)][m] for m in METRICS])

    print("connections | coroed rps | threads rps | coroed p99 | threads p99 | coroed MB | threads MB | errors c/t")
    for c in sorted({c for _, c in data}):
        a, b = data.get(("coroed", c)), data.get(("threads", c))
        if a and b:
            print(f"{c:>11} | {a['rps']:>10.0f} | {b['rps']:>11.0f} | {a['p99_ms']:>10.2f} | "
                  f"{b['p99_ms']:>11.2f} | {a['rss_mb']:>9.0f} | {b['rss_mb']:>10.0f} | "
                  f"{a['errors']:.0f}/{b['errors']:.0f}")


if __name__ == "__main__":
    main()
