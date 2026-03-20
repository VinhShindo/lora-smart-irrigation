const socket = io();
const charts = {};
const nodeControlState = {};
const watchdogTimers = {};

Chart.defaults.color = '#64748b';
Chart.defaults.borderColor = 'rgba(148, 163, 184, 0.05)';
Chart.defaults.font.family = 'Inter';
Chart.defaults.font.size = 10;

function initNodeState(nodeId) {
    const el = document.getElementById(nodeId);
    nodeControlState[nodeId] = {
        state: "IDLE",
        cmdId: null,
        statusEl: el ? el.querySelector(".pending-msg") : null
    };
}

function getStatusEl(nodeId) {
    return nodeControlState[nodeId]?.statusEl;
}

function setState(nodeId, newState) {
    nodeControlState[nodeId].state = newState;
}

function getState(nodeId) {
    return nodeControlState[nodeId]?.state || "IDLE";
}

function setCmd(nodeId, cmdId) {
    nodeControlState[nodeId].cmdId = cmdId;
}

function getCmd(nodeId) {
    return nodeControlState[nodeId]?.cmdId;
}

function clearCmd(nodeId) {
    nodeControlState[nodeId].cmdId = null;
}

function disableControls(el, state) {
    el.querySelectorAll("button").forEach(b => b.disabled = state);
}

socket.on("connect", () => {
    {% for node in nodes %}
    socket.emit("subscribe_node", { node_id: "{{node}}" });
    initNodeState("{{node}}");
    {% endfor %}
});

function createMiniChart(id, color) {
    const ctx = document.getElementById(id).getContext('2d');
    const gradient = ctx.createLinearGradient(0, 0, 0, 100);
    gradient.addColorStop(0, color + '50');
    gradient.addColorStop(1, color + '00');

    return new Chart(ctx, {
        type: 'line',
        data: { labels: [], datasets: [{ data: [], borderColor: color, borderWidth: 1.5, pointRadius: 0, tension: 0.4, fill: true, backgroundColor: gradient }] },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            plugins: { legend: { display: false } },
            scales: {
                x: { display: false },
                y: { display: true, position: 'right', ticks: { count: 3, font: { size: 9 }, color: '#475569' }, grid: { display: false } }
            },
            animation: false
        }
    });
}

function updateCharts(nodeId) {
    fetch(`/api/node/${nodeId}/measurements`)
        .then(r => r.json())
        .then(data => {
            if (!data || !data.length) return;
            data.reverse();
            const last = data[data.length - 1];
            const el = document.getElementById(nodeId);

            el.querySelector(".temp-val").innerText = last.temp + "°C";
            el.querySelector(".humi-val").innerText = last.humi + "%";
            el.querySelector(".soil-val").innerText = last.soil + "%";
            el.querySelector(".light-val").innerText = last.light;

            if (!charts[nodeId]) {
                charts[nodeId] = {
                    temp: createMiniChart(`chart-temp-${nodeId}`, '#fca5a5'),
                    humi: createMiniChart(`chart-humi-${nodeId}`, '#93c5fd'),
                    soil: createMiniChart(`chart-soil-${nodeId}`, '#86efac'),
                    light: createMiniChart(`chart-light-${nodeId}`, '#fcd34d')
                };
            }

            const labels = data.map(() => "");
            const c = charts[nodeId];

            c.temp.data.labels = labels; c.temp.data.datasets[0].data = data.map(d => d.temp); c.temp.update();
            c.humi.data.labels = labels; c.humi.data.datasets[0].data = data.map(d => d.humi); c.humi.update();
            c.soil.data.labels = labels; c.soil.data.datasets[0].data = data.map(d => d.soil); c.soil.update();
            c.light.data.labels = labels; c.light.data.datasets[0].data = data.map(d => d.light); c.light.update();
        });
}

function sendControl(nodeId, action) {
    const el = document.getElementById(nodeId);
    if (!el) return;

    if (!nodeControlState[nodeId])
        initNodeState(nodeId);

    if (el.classList.contains("offline")) return;

    console.log("STATE BEFORE SEND:", nodeControlState[nodeId]);
    if (getState(nodeId) !== "IDLE") return;

    const cmdId = Date.now().toString();
    const statusMsg = getStatusEl(nodeId);;
    if (!statusMsg) return;

    setState(nodeId, "SENDING");
    setCmd(nodeId, cmdId);

    disableControls(el, true);

    statusMsg.innerText = `ĐANG GỬI LỆNH ${action}...`;
    statusMsg.className = "status-msg status-waiting";
    statusMsg.style.color = "#f59e0b";

    socket.emit("control", { node_id: nodeId, action, cmd_id: cmdId });

    if (watchdogTimers[nodeId]) clearTimeout(watchdogTimers[nodeId]);

    watchdogTimers[nodeId] = setTimeout(() => {

        if (getState(nodeId) !== "SENDING" &&
            getState(nodeId) !== "EXECUTING") return;

        setState(nodeId, "TIMEOUT");
        clearCmd(nodeId);
        delete watchdogTimers[nodeId];

        statusMsg.innerText = "TIMEOUT";
        statusMsg.className = "status-msg";
        statusMsg.style.color = "#ef4444";

        if (!el.classList.contains("offline"))
            disableControls(el, false);

        setTimeout(() => {
            setState(nodeId, "IDLE");
            statusMsg.innerText = "SYSTEM READY";
            statusMsg.className = "status-msg";
            statusMsg.style.color = "";
        }, 1500);

    }, 8000);
}

socket.on("node_pending", data => {
    const nodeId = data.node_id;
    const el = document.getElementById(nodeId);
    if (!el) return;
    if (data.cmd_id !== getCmd(nodeId)) return;
    const statusMsg = getStatusEl(nodeId);
    if (!statusMsg) return;

    if (data.queue_pos && data.queue_pos > 1) {
        setState(nodeId, "QUEUED");
        statusMsg.innerText = "ĐANG XẾP HÀNG (#" + data.queue_pos + ")";
        statusMsg.className = "status-msg status-waiting";
    } else {
        setState(nodeId, "EXECUTING");
        statusMsg.innerText = "EXECUTING...";
        statusMsg.className = "status-msg status-executing";
    }
});

socket.on("node_ack", data => {
    const nodeId = data.node_id;
    const el = document.getElementById(nodeId);
    if (!el) return;

    const currentCmd = getCmd(nodeId);
    if (!currentCmd) return;
    if (data.cmd_id !== currentCmd) return;

    if (watchdogTimers[nodeId]) {
        clearTimeout(watchdogTimers[nodeId]);
        delete watchdogTimers[nodeId];
    }

    const statusMsg = getStatusEl(nodeId);;
    if (!statusMsg) return;

    if (data.pump !== undefined)
        el.querySelector(".pump").innerText = data.pump;

    if (data.mode !== undefined) {
        const modeEl = el.querySelector(".mode");
        modeEl.innerText = data.mode;
        modeEl.style.color =
            data.mode === "SEN"
                ? "var(--accent-blue)"
                : "var(--accent-amber)";
    }

    if (data.amp !== undefined && data.amp !== null)
        el.querySelector(".amp").innerText = data.amp + " A";

    if (data.flow !== undefined && data.flow !== null)
        el.querySelector(".flow").innerText = data.flow + " L/m";

    if (data.last_soil !== undefined && data.last_soil !== null)
        el.querySelector(".last-soil").innerText = data.last_soil + "%";

    if (data.server_time) {
        const t = new Date(data.server_time).toLocaleTimeString();
        el.querySelector(".device-updated").innerText = t;
        el.querySelector(".time").innerText = t;
    }

    clearCmd(nodeId);
    setState(nodeId, "IDLE");

    disableControls(el, false);

    if (data.success === false) {
        statusMsg.innerText = data.message || "THẤT BẠI!";
        statusMsg.className = "status-msg";
        statusMsg.style.color = "#ef4444";
    } else {
        statusMsg.innerText = "THÀNH CÔNG!";
        statusMsg.className = "status-msg";
        statusMsg.style.color = "#10b981";
    }

    setTimeout(() => {
        statusMsg.innerText = "SYSTEM READY";
        statusMsg.className = "status-msg";
        statusMsg.style.color = "";
    }, 1500);
});

socket.on("node_realtime", data => {
    const el = document.getElementById(data.node_id);
    if (!el) return;

    el.querySelector(".rssi").innerText = (data.rssi ?? "--") + " dBm";
    el.querySelector(".amp").innerText = (data.amp ?? "--") + " A";
    el.querySelector(".flow").innerText = (data.flow ?? "--") + " L/m";
    el.querySelector(".pump").innerText = data.pump || "OFF";
    el.querySelector(".mode").innerText = data.mode || "--";
    el.querySelector(".duration").innerText = (data.duration_minutes || 0) + " m";

    if (data.last_soil !== undefined && data.last_soil !== null)
        el.querySelector(".last-soil").innerText = data.last_soil + "%";

    const timeStr = data.updated_at
        ? new Date(data.updated_at).toLocaleTimeString()
        : "--:--:--";

    el.querySelector(".device-updated").innerText = timeStr;
    el.querySelector(".time").innerText = timeStr;

    const modeEl = el.querySelector(".mode");
    modeEl.style.color =
        data.mode === "SEN"
            ? "var(--accent-blue)"
            : "var(--accent-amber)";

    if (data.current_status === "OFFLINE") {
        el.classList.add("offline");
        el.querySelector(".node-status-text").innerText = "OFFLINE";
        el.querySelector(".node-status-text").className = "status-badge badge-offline node-status-text";
        disableControls(el, true);
    }

    if (data.current_status === "ONLINE") {
        el.classList.remove("offline");
        el.querySelector(".node-status-text").innerText = "ONLINE";
        el.querySelector(".node-status-text").className = "status-badge badge-online node-status-text";

        if (getState(data.node_id) === "IDLE")
            disableControls(el, false);
    }
});

setInterval(() => {
    {% for node in nodes %} updateCharts("{{node}}"); {% endfor %}
}, 10000);

window.onload = () => {
    {% for node in nodes %} updateCharts("{{node}}"); {% endfor %}
};
