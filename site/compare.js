/* The ranking page: one test at a time, every processor that has run it. */
'use strict';

const params = new URLSearchParams(location.search);
const state = {
    runs: [],
    test: metricById(params.get('test')) ? params.get('test') : 'mc_k2',
    picks: (params.get('pick') || '').split(',').filter(Boolean).slice(0, 2),
};
const $ = (id) => document.getElementById(id);

function yours() {
    return state.runs.find((run) => run.source === 'yours') ?? null;
}

function passesFilters(run) {
    const isa = $('filter-isa').value;
    const os = $('filter-os').value;
    const power = $('filter-power').value;
    if (isa && run.isa !== isa) return false;
    if (os && run.os !== os) return false;
    if (power === 'battery' && run.ac !== false) return false;
    if (power === 'mains' && run.ac === false) return false;
    return true;
}

function keepInAddress() {
    const query = new URLSearchParams({ test: state.test });
    const shareable = state.picks.filter((id) => id !== 'yours');
    if (shareable.length) query.set('pick', shareable.join(','));
    history.replaceState(null, '', `?${query}`);
}

function renderGroups() {
    const current = metricById(state.test).group;
    $('groups').replaceChildren(...GROUPS.map((group, i) => el('button', {
        class: `tab${group.id === current ? ' current' : ''}`,
        type: 'button',
        'aria-pressed': String(group.id === current),
        onclick: () => {
            state.test = METRICS.find((metric) => metric.group === group.id).id;
            render();
        },
    }, el('span', { class: 'label' }, `0${i + 1}`), el('span', {}, group.name))));
}

function renderTests() {
    const metric = metricById(state.test);
    const group = GROUPS.find((g) => g.id === metric.group);
    $('tests-title').textContent = `Tests in ${group.name}`;
    $('tests').replaceChildren(...METRICS.filter((m) => m.group === group.id).map((m) => el('button', {
        class: `test-button${m.id === state.test ? ' current' : ''}`,
        type: 'button',
        'aria-pressed': String(m.id === state.test),
        onclick: () => {
            state.test = m.id;
            render();
        },
    }, el('span', { class: 'test-name' }, m.name), el('span', { class: 'label' }, m.unit))));
}

function togglePick(id) {
    state.picks = state.picks.includes(id)
        ? state.picks.filter((pick) => pick !== id)
        : state.picks.concat(id).slice(-2);
    render();
}

function rankedRows(metric) {
    const speed = (value) => (metric.higher ? value.v : 1 / value.v);
    return state.runs
        .filter((run) => rankable(run) && passesFilters(run) && run.metrics[metric.id] && !run.metrics[metric.id].failed)
        .map((run) => ({ run, value: run.metrics[metric.id], speed: speed(run.metrics[metric.id]) }))
        .sort((a, b) => b.speed - a.speed);
}

function againstText(metric, value, base) {
    const result = compareValues(metric, value, base);
    if (!result.winner) return 'same';
    return `${result.size} ${result.winner === 'a' ? 'better' : 'worse'}${result.noise ? ', may be noise' : ''}`;
}

function renderRows() {
    const metric = metricById(state.test);
    $('test-title').textContent = metric.title;
    $('test-sub').textContent = metric.sub;
    $('test-unit').textContent = `${metric.unit} · ${metric.higher ? 'higher' : 'lower'} is better`;

    const rows = rankedRows(metric);
    if (!rows.length) {
        const noFilters = !$('filter-isa').value && !$('filter-os').value && !$('filter-power').value;
        const failed = yours()?.metrics[metric.id]?.failed;
        let message = 'No processor matches these filters on this test.';
        if (failed) message = 'Your result computed wrong answers on this test, so it is left out.';
        else if (noFilters) message = 'No reference has run this test yet. Open a result of yours that has.';
        $('rows').replaceChildren(el('p', { class: 'rows-empty label' }, message));
        return;
    }

    const mine = rows.find((row) => row.run.source === 'yours');
    const base = mine ?? rows[0];
    const fastest = rows[0].speed;
    const head = el('div', { class: 'rank-row rank-head label muted' },
        el('span', {}, 'Pick'), el('span', {}, '#'), el('span', {}, 'Processor'), el('span', {}, 'Speed'),
        el('span', { class: 'right' }, metric.unit), el('span', { class: 'right' }, mine ? 'vs. yours' : 'vs. fastest'));

    $('rows').replaceChildren(head, ...rows.map((row, i) => {
        const { run, value } = row;
        const isMine = run.source === 'yours';
        const picked = state.picks.includes(run.id);
        const tags = valueTags(value);
        return el('div', { class: `rank-row${isMine ? ' mine' : ''}${picked ? ' picked' : ''}` },
            el('input', {
                type: 'checkbox',
                checked: picked,
                'aria-label': `Pick ${run.name}, ${describe(run)}, to compare`,
                onchange: () => togglePick(run.id),
            }),
            el('span', { class: 'rank-number label' }, String(i + 1).padStart(2, '0')),
            el('span', { class: 'rank-name' },
                el('b', {}, isMine ? `${run.name} · yours` : run.name),
                el('span', { class: 'label' }, [describe(run), ...tags].join(' · '))),
            el('span', { class: 'rank-bar' }, el('span', {
                class: isMine ? 'fill mine' : 'fill',
                style: `width: ${Math.max(1.5, (row.speed / fastest) * 100)}%`,
            })),
            el('span', { class: 'rank-score right' }, formatValue(value.v)),
            el('span', { class: 'rank-against right label' }, row === base ? '—' : againstText(metric, value, base.value)));
    }));
}

function renderSelection() {
    const picked = state.picks.map((id) => state.runs.find((run) => run.id === id)).filter(Boolean);
    $('picked-count').textContent = `${picked.length} of 2 picked`;
    $('picked-names').textContent = picked.length
        ? picked.map((run) => `${run.name} (${osName(run)})`).join('  vs.  ')
        : 'Tick two processors to compare them test by test';
    const query = new URLSearchParams();
    if (picked[0]) query.set('a', picked[0].id);
    if (picked[1]) query.set('b', picked[1].id);
    $('versus-link').href = `versus.html${picked.length ? `?${query}` : ''}`;
}

function renderYours() {
    const run = yours();
    $('yours-name').textContent = run ? run.name : 'No result of yours yet';
    $('yours-open-text').textContent = run ? 'Open another…' : 'Open a result…';
    $('yours-remove').hidden = !run;
    if (!run) {
        $('yours-note').textContent = 'Open a result file from the Prismark app to see it in every ranking.';
    } else if (run.wrong) {
        $('yours-note').textContent = 'Some of its results came out wrong, so it is left out of the rankings.';
    } else if (run.quick) {
        $('yours-note').textContent = 'A quick run: its numbers are rough, so it is not ranked. Run the full tests in the app.';
    } else {
        $('yours-note').textContent = `${describe(run)}${run.date ? ` · ${run.date}` : ''}. Highlighted in every ranking.`;
    }
}

function render() {
    $('count').textContent = `Compare · ${state.runs.filter((run) => run.source === 'reference').length} reference results`;
    keepInAddress();
    renderGroups();
    renderTests();
    renderRows();
    renderSelection();
    renderYours();
}

/* ---------- your own result ---------- */

async function openFile(file) {
    $('yours-error').hidden = true;
    try {
        const run = await readResultFile(file);
        state.runs = state.runs.filter((other) => other.source !== 'yours').concat(run);
        saveYours(run);
        render();
    } catch (error) {
        $('yours-error').textContent = error.message;
        $('yours-error').hidden = false;
    }
}

$('yours-file').addEventListener('change', (event) => {
    if (event.target.files[0]) openFile(event.target.files[0]);
    event.target.value = '';
});

$('yours-remove').addEventListener('click', () => {
    state.runs = state.runs.filter((run) => run.source !== 'yours');
    state.picks = state.picks.filter((id) => id !== 'yours');
    saveYours(null);
    render();
});

const dropZone = $('yours');
dropZone.addEventListener('dragover', (event) => {
    event.preventDefault();
    dropZone.classList.add('dropping');
});
dropZone.addEventListener('dragleave', () => dropZone.classList.remove('dropping'));
dropZone.addEventListener('drop', (event) => {
    event.preventDefault();
    dropZone.classList.remove('dropping');
    if (event.dataTransfer.files[0]) openFile(event.dataTransfer.files[0]);
});

['filter-isa', 'filter-os', 'filter-power'].forEach((id) => $(id).addEventListener('change', render));

/* ---------- start ---------- */

loadReferences()
    .then((references) => {
        const mine = loadYours();
        state.runs = mine ? references.concat(mine) : references;
        render();
    })
    .catch(() => {
        $('test-title').textContent = 'The results could not be loaded';
        $('rows').replaceChildren(el('p', { class: 'rows-empty label' }, 'Reload the page to try again.'));
    });
