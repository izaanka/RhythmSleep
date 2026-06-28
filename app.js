// ============================================================================
// RhythmSleep — Web Dashboard Logic
// ============================================================================

// 5-state brainwave mapping for the chart Y-axis
const stateMapping = {
    "Deep Sleep":   1,
    "Light Sleep":  2,
    "Relaxed":      3,
    "Active":       4,
    "Focused":      5
};

// Colors for each band
const bandColors = {
    "Deep Sleep":   "#6366f1",
    "Light Sleep":  "#a78bfa",
    "Relaxed":      "#22d3ee",
    "Active":       "#f97316",
    "Focused":      "#f43f5e"
};

let currentChart = null;
let livePollingInterval = null;

// ============================================================================
// INITIALIZATION
// ============================================================================
async function initializeMenu() {
    try {
        const response = await fetch('/api/files');
        const files = await response.json();
        const selectElement = document.getElementById('fileSelect');

        // Clear existing options
        selectElement.innerHTML = '';

        if (files.length === 0) {
            const option = document.createElement('option');
            option.text = "No logs found";
            selectElement.add(option);
            return;
        }

        files.forEach(file => {
            const option = document.createElement('option');
            option.value = file;
            // Format: "sleep_log_2026-06-28.csv" → "2026-06-28"
            option.text = file.replace('sleep_log_', '').replace('.csv', '');
            selectElement.add(option);
        });

        fetchAndRender(selectElement.value);

        selectElement.addEventListener('change', (e) => {
            fetchAndRender(e.target.value);
        });
    } catch (e) {
        console.error("Failed to load file list:", e);
    }
}

// ============================================================================
// FETCH & RENDER CHART
// ============================================================================
async function fetchAndRender(filename) {
    if (!filename) return;

    try {
        const response = await fetch(`/api/data?file=${filename}`);
        const data = await response.json();

        if (data.timestamps.length === 0) {
            document.getElementById('sleepScore').innerText = "--";
            document.getElementById('totalTime').innerText = "0h 0m";
            document.getElementById('dominantState').innerText = "--";
            if (currentChart) currentChart.destroy();
            return;
        }

        // ── Calculate Metrics ───────────────────────────────────────────
        let stateDurations = {
            "Deep Sleep": 0, "Light Sleep": 0, "Relaxed": 0, "Active": 0, "Focused": 0
        };
        let totalMinutes = 0;

        for (let i = 0; i < data.timestamps.length - 1; i++) {
            let t1 = new Date(data.timestamps[i].replace(' ', 'T'));
            let t2 = new Date(data.timestamps[i + 1].replace(' ', 'T'));
            let diffMins = (t2 - t1) / 60000;

            // Cap at 5 minutes to avoid gaps inflating durations
            diffMins = Math.min(diffMins, 5);
            totalMinutes += diffMins;

            let state = data.states[i];
            if (stateDurations.hasOwnProperty(state)) {
                stateDurations[state] += diffMins;
            }
        }

        // Total time
        let hours = Math.floor(totalMinutes / 60);
        let mins = Math.round(totalMinutes % 60);
        document.getElementById('totalTime').innerText = `${hours}h ${mins}m`;

        // Dominant state
        let dominantState = "--";
        let maxDuration = 0;
        for (const [state, duration] of Object.entries(stateDurations)) {
            if (duration > maxDuration) {
                maxDuration = duration;
                dominantState = state;
            }
        }
        document.getElementById('dominantState').innerText = dominantState;

        // Sleep score: weighted by restfulness
        let score = 0;
        if (totalMinutes > 0) {
            let restfulMinutes =
                (stateDurations["Deep Sleep"] * 2.0) +
                (stateDurations["Light Sleep"] * 1.5) +
                (stateDurations["Relaxed"] * 1.0) +
                (stateDurations["Active"] * 0.3) +
                (stateDurations["Focused"] * 0.1);
            let rawScore = (restfulMinutes / totalMinutes) * 55;
            score = Math.max(0, Math.min(100, Math.round(rawScore)));
        }
        document.getElementById('sleepScore').innerText = score;

        // ── Build Chart Data ────────────────────────────────────────────
        const coordinateData = data.timestamps.map((timestamp, index) => {
            let state = data.states[index];
            return {
                x: new Date(timestamp.replace(' ', 'T')),
                y: stateMapping[state] || 0
            };
        });

        // Color segments based on state
        const segmentColors = data.states.map(state => bandColors[state] || '#64748b');

        if (currentChart) {
            currentChart.destroy();
        }

        const ctx = document.getElementById('sleepChart').getContext('2d');
        currentChart = new Chart(ctx, {
            type: 'line',
            data: {
                datasets: [{
                    label: 'Brainwave State',
                    data: coordinateData,
                    borderColor: function(context) {
                        if (!context.p0) return '#6366f1';
                        return segmentColors[context.p0DataIndex] || '#6366f1';
                    },
                    segment: {
                        borderColor: function(ctx) {
                            return segmentColors[ctx.p0DataIndex] || '#6366f1';
                        }
                    },
                    backgroundColor: 'rgba(99, 102, 241, 0.08)',
                    borderWidth: 2.5,
                    stepped: 'before',
                    fill: true,
                    pointRadius: 0,
                    pointHoverRadius: 4,
                    pointHoverBackgroundColor: '#e2e8f0'
                }]
            },
            options: {
                responsive: true,
                maintainAspectRatio: true,
                interaction: {
                    mode: 'nearest',
                    intersect: false
                },
                scales: {
                    y: {
                        ticks: {
                            callback: function(value) {
                                return Object.keys(stateMapping).find(key => stateMapping[key] === value) || "";
                            },
                            color: '#64748b',
                            font: { family: 'Inter', size: 11 }
                        },
                        min: 0.5,
                        max: 5.5,
                        grid: {
                            color: 'rgba(255, 255, 255, 0.04)'
                        }
                    },
                    x: {
                        type: 'time',
                        time: {
                            unit: 'minute',
                            displayFormats: { minute: 'HH:mm' },
                            tooltipFormat: 'HH:mm:ss'
                        },
                        ticks: {
                            color: '#64748b',
                            maxTicksLimit: 18,
                            font: { family: 'Inter', size: 11 }
                        },
                        grid: {
                            color: 'rgba(255, 255, 255, 0.04)'
                        }
                    }
                },
                plugins: {
                    legend: {
                        display: false
                    },
                    tooltip: {
                        backgroundColor: 'rgba(15, 23, 42, 0.95)',
                        titleFont: { family: 'Inter' },
                        bodyFont: { family: 'Inter' },
                        borderColor: 'rgba(255,255,255,0.1)',
                        borderWidth: 1,
                        padding: 12,
                        callbacks: {
                            label: function(context) {
                                let idx = context.dataIndex;
                                let state = data.states[idx] || "Unknown";
                                let freq = data.frequencies[idx] || 0;
                                return `${state} — ${freq.toFixed(1)} Hz`;
                            }
                        }
                    }
                }
            }
        });

    } catch (e) {
        console.error("Failed to fetch data:", e);
    }
}

// ============================================================================
// LIVE DATA POLLING
// ============================================================================
async function pollLiveData() {
    try {
        const response = await fetch('/api/live');
        const data = await response.json();

        // Update live frequency
        let freqEl = document.getElementById('liveFreq');
        if (data.frequency > 0) {
            freqEl.innerHTML = `${data.frequency.toFixed(1)}<span class="unit">Hz</span>`;
        } else {
            freqEl.innerHTML = `--<span class="unit">Hz</span>`;
        }

        // Update live band with color
        let bandEl = document.getElementById('liveBand');
        bandEl.innerText = data.state;
        let bandCard = document.getElementById('liveBandCard');
        // Apply band-specific color class
        bandCard.className = 'live-card';
        if (data.state === "Focused") bandCard.classList.add('band-focused');
        else if (data.state === "Active") bandCard.classList.add('band-active');
        else if (data.state === "Relaxed") bandCard.classList.add('band-relaxed');
        else if (data.state === "Light Sleep") bandCard.classList.add('band-light');
        else if (data.state === "Deep Sleep") bandCard.classList.add('band-deep');

        // Update alarm display
        let alarmHour = Math.floor(data.alarm_m / 60);
        let alarmMin = data.alarm_m % 60;
        document.getElementById('liveAlarm').innerText =
            `${String(alarmHour).padStart(2, '0')}:${String(alarmMin).padStart(2, '0')}`;

        // Update alarm status
        let statusEl = document.getElementById('liveAlarmStatus');
        if (data.alarm_triggered) {
            statusEl.innerText = "TRIGGERED";
            statusEl.style.color = "#f43f5e";
        } else {
            statusEl.innerText = "Standby";
            statusEl.style.color = "#22c55e";
        }

    } catch (e) {
        // Silently handle polling errors
    }
}

// ============================================================================
// ALARM CONFIGURATION
// ============================================================================
async function setAlarm() {
    let hour = parseInt(document.getElementById('alarmHour').value) || 0;
    let minute = parseInt(document.getElementById('alarmMin').value) || 0;
    let buffer = parseInt(document.getElementById('bufferMin').value) || 30;

    // Validate
    hour = Math.max(0, Math.min(23, hour));
    minute = Math.max(0, Math.min(59, minute));
    buffer = Math.max(5, Math.min(120, buffer));

    let alarm_m = hour * 60 + minute;

    let statusEl = document.getElementById('alarmStatusMsg');

    try {
        const response = await fetch('/api/alarm', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ alarm_m: alarm_m, buffer_m: buffer })
        });

        if (response.ok) {
            const result = await response.json();
            statusEl.className = 'alarm-status success';
            statusEl.innerText = `✓ Alarm set: ${String(hour).padStart(2, '0')}:${String(minute).padStart(2, '0')} ±${buffer}m`;

            // Update the live alarm display
            document.getElementById('liveAlarm').innerText =
                `${String(hour).padStart(2, '0')}:${String(minute).padStart(2, '0')}`;

            // Clear success message after 5 seconds
            setTimeout(() => { statusEl.innerText = ''; }, 5000);
        } else {
            statusEl.className = 'alarm-status error';
            statusEl.innerText = '✗ Failed to set alarm';
        }
    } catch (e) {
        statusEl.className = 'alarm-status error';
        statusEl.innerText = '✗ Connection error';
    }
}

// ============================================================================
// LOAD ALARM SETTINGS ON PAGE LOAD
// ============================================================================
async function loadAlarmSettings() {
    try {
        const response = await fetch('/api/alarm');
        const data = await response.json();
        document.getElementById('alarmHour').value = data.alarm_hour;
        document.getElementById('alarmMin').value = data.alarm_min;
        document.getElementById('bufferMin').value = data.buffer_m;
    } catch (e) {
        // Use defaults
    }
}

// ============================================================================
// BOOT SEQUENCE
// ============================================================================
initializeMenu();
loadAlarmSettings();

// Poll live data every 2 seconds
livePollingInterval = setInterval(pollLiveData, 2000);
// Initial poll
pollLiveData();
