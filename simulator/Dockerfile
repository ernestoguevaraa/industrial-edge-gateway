# syntax=docker/dockerfile:1

FROM python:3.12-slim AS builder
WORKDIR /build
RUN python -m venv /opt/venv
ENV PATH=/opt/venv/bin:$PATH
COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt
COPY pyproject.toml README.md ./
COPY src ./src
# --no-deps: the runtime tree comes only from the exact pins above; pip check
# fails the build if those pins are incomplete or inconsistent.
RUN pip install --no-cache-dir --no-deps . && pip check

FROM python:3.12-slim
RUN useradd --system --uid 10001 --no-create-home --shell /usr/sbin/nologin simulator
COPY --from=builder /opt/venv /opt/venv
ENV PATH=/opt/venv/bin:$PATH \
    PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    SIM_HOST=0.0.0.0 \
    SIM_PORT=4840 \
    SIM_TICK_MS=500 \
    SIM_SEED=42 \
    SIM_QUALITY_EVENTS=1 \
    SIM_LOG_LEVEL=INFO \
    SIM_LOG_FORMAT=json
USER simulator
EXPOSE 4840

# Basic liveness only: proves the process is listening on the TCP port.
# It does not perform an OPC UA session or read, so it is not proof that reads work.
HEALTHCHECK --interval=10s --timeout=3s --start-period=10s --retries=3 \
    CMD ["python", "-c", "import os, socket; socket.create_connection(('127.0.0.1', int(os.environ['SIM_PORT'])), timeout=2).close()"]

ENTRYPOINT ["python", "-m", "plant_simulator"]
