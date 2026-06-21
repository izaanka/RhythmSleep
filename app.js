// Strictly map the exact CSV strings to y-axis coordinates
const stateMapping = { "Deep": 1, "Core": 2, "REM": 3, "Awake": 4 };
let currentChart = null; 

async function initializeMenu() {
    const response = await fetch('/api/files');
    const files = await response.json();
    const selectElement = document.getElementById('fileSelect');
    
    if (files.length === 0) {
        const option = document.createElement('option');
        option.text = "No logs found";
        selectElement.add(option);
        return;
    }

    files.forEach(file => {
        const option = document.createElement('option');
        option.value = file;
        option.text = file.replace('sleep_log_', '').replace('.csv', '');
        selectElement.add(option);
    });

    fetchAndRender(selectElement.value);

    selectElement.addEventListener('change', (e) => {
        fetchAndRender(e.target.value);
    });
}

async function fetchAndRender(filename) {
    if (!filename) return;

    const response = await fetch(`/api/data?file=${filename}`);
    const data = await response.json();
    
    if(data.timestamps.length === 0) {
        document.getElementById('sleepScore').innerText = "--";
        document.getElementById('totalTime').innerText = "0h 0m";
        if(currentChart) currentChart.destroy();
        return;
    }

    let deepMinutes = 0, lightMinutes = 0, wakeCount = 0;
    let totalMinutes = 0;

    // Calculate exact time deltas between phase shifts
    for (let i = 0; i < data.timestamps.length - 1; i++) {
        let t1 = new Date(data.timestamps[i].replace(' ', 'T'));
        let t2 = new Date(data.timestamps[i+1].replace(' ', 'T'));
        let diffMins = (t2 - t1) / 60000;
        
        totalMinutes += diffMins;
        
        let currentState = data.states[i];
        if (currentState === "Deep") deepMinutes += diffMins;
        if (currentState === "Core" || currentState === "REM") lightMinutes += diffMins;
        if (currentState === "Awake") {
            if (i === 0 || data.states[i-1] !== "Awake") wakeCount++;
        }
    }

    // Map textual states to numerical Cartesian coordinates
    const numericStates = data.states.map(state => stateMapping[state] || 0);

    document.getElementById('totalTime').innerText = `${Math.floor(totalMinutes / 60)}h ${totalMinutes % 60}m`;
    
    let score = 0;
    if (totalMinutes > 0) {
        let rawScore = (((1.5 * deepMinutes) + (1.0 * lightMinutes)) / totalMinutes) * 100 - (5 * wakeCount);
        score = Math.max(0, Math.min(100, Math.round(rawScore)));
    }
    document.getElementById('sleepScore').innerText = score;

    if (currentChart) {
        currentChart.destroy();
    }

    const ctx = document.getElementById('sleepChart').getContext('2d');
    currentChart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: data.timestamps.map(t => t.split(' ')[1]),
            datasets: [{
                label: 'Neurological State',
                data: numericStates,
                borderColor: '#ffffff',
                backgroundColor: 'rgba(255, 255, 255, 0.2)',
                borderWidth: 3,
                stepped: true,
                fill: true,
                pointRadius: 0
            }]
        },
        options: {
            responsive: true,
            scales: {
                y: {
                    ticks: {
                        callback: function(value) {
                            return Object.keys(stateMapping).find(key => stateMapping[key] === value) || "";
                        },
                        color: '#ffffff'
                    },
                    min: 0.5,
                    max: 4.5
                },
                x: { ticks: { color: '#ffffff', maxTicksLimit: 20 } }
            },
            plugins: { legend: { labels: { color: '#ffffff' } } }
        }
    });
}

// Boot sequence initialization
initializeMenu();
