# syntax=docker/dockerfile:1
#
# Deterministic Modbus TCP device simulator (demo only, never part of the gateway image).
# Standard library only: no third-party runtime dependencies.

FROM python:3.12-slim AS builder
WORKDIR /build
RUN python -m venv /opt/venv
ENV PATH=/opt/venv/bin:$PATH
COPY pyproject.toml README.md ./
COPY src ./src
RUN pip install --no-cache-dir --no-deps . && pip check

FROM python:3.12-slim
RUN useradd --system --uid 10003 --no-create-home --shell /usr/sbin/nologin simulator
COPY --from=builder /opt/venv /opt/venv
ENV PATH=/opt/venv/bin:$PATH \
    PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    MODBUS_SIM_HOST=0.0.0.0 \
    MODBUS_SIM_PORT=5020 \
    MODBUS_SIM_UNIT_ID=1 \
    MODBUS_SIM_TICK_MS=1000 \
    MODBUS_SIM_LOG_LEVEL=INFO \
    MODBUS_SIM_LOG_FORMAT=json
USER simulator
EXPOSE 5020

# A real Modbus TCP read (FC03 of holding register 0), not just a TCP connect.
HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
    CMD ["python", "-m", "modbus_simulator.healthcheck"]

ENTRYPOINT ["python", "-m", "modbus_simulator"]
