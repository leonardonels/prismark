/* The head-to-head page: two processors, every test, and never a total. */
'use strict';

const params = new URLSearchParams(location.search);
const state = { runs: [], a: params.get('a'), b: params.get('b') };
const $ = (id) => document.getElementById(id);
const run = (id) => state.runs.find((r) => r.id === id);

function keepInAddress() {
    history.replaceState(null, '', `?${new URLSearchParams({ a: state.a, b: state.b })}`);
    const test = METRICS.find((metric) => run(state.a).metrics[metric.id] && run(state.b).metrics[metric.id]);
    const picks = [state.a, state.b].filter((id) => id !== 'yours').join(',');
    $('back').href = `compare.html?${new URLSearchParams({ test: test ? test.id : 'mc_k2', pick: picks })}`;
}

/* What a row calls a side: its processor without the maker, or its letter when both are the same processor. */
function sideNames(a, b) {
    const short = (name) => name.replace(/^(AMD|Intel|Apple)\s+/, '');
    const nameA = short(a.name);
    const nameB = short(b.name);
    return nameA === nameB ? { a: 'A', b: 'B' } : { a: nameA, b: nameB };
}

function cacheName(bytes) {
    return bytes ? `${Math.round(bytes / (1 << 20))} MB` : 'Not reported';
}

function renderPass(side, id) {
    const current = run(id);
    const isMine = current.source === 'yours';
    const facts = [
        ['Threads', String(current.threads)],
        ['Largest cache', cacheName(current.cache)],
        ['System', `${osName(current)} · ${isaName(current)}`],
        ['Power', current.powerMode ? `${powerName(current)} · ${current.powerMode}` : powerName(current)],
    ];
    const choose = el('select', {
        'aria-label': `Processor on side ${side}`,
        onchange: (event) => {
            state[side.toLowerCase()] = event.target.value;
            render();
        },
    }, ...state.runs.filter(rankable).map((other) => el('option', {
        value: other.id,
        selected: other.id === id,
    }, `${other.name}, ${osName(other)}${other.ac === false ? ', on battery' : ''}${other.source === 'yours' ? ' (yours)' : ''}`)));

    /* Volt marks each test's winner on this page, so no pass is volt: "yours" is said in words. */
    return el('div', { class: 'panel pass-card span-6 dark' },
        el('div', { class: 'pass-card-main' },
            el('div', { class: 'meta label' },
                el('span', {}, `${side} · ${isMine ? 'yours' : 'reference'}`),
                el('span', {}, current.date ?? '')),
            el('div', { class: 'pass-card-title' },
                el('h2', { class: 'heading' }, current.name),
                el('span', { class: 'pass-ghost', 'aria-hidden': 'true' }, side)),
            choose,
            el('div', { class: 'facts' }, ...facts.map(([key, value]) => el('span', { class: 'fact' },
                el('span', { class: 'label' }, key),
                el('b', {}, value))))),
        el('div', { class: 'pass-card-stub label' }, isMine
            ? 'Read from your result file · not uploaded'
            : `Measured with Prismark ${current.version ?? ''}`));
}

function renderRow(metric, a, b, names) {
    const valueA = a.metrics[metric.id];
    const valueB = b.metrics[metric.id];
    const both = valueA && valueB;
    const speed = (value) => (metric.higher ? value.v : 1 / value.v);
    const top = both ? Math.max(speed(valueA), speed(valueB)) : 1;

    let verdict;
    const lit = { a: '', b: '' }; /* how each side's half of the row is marked */
    if (valueA?.failed || valueB?.failed) {
        const failed = [valueA?.failed && names.a, valueB?.failed && names.b].filter(Boolean).join(' and ');
        verdict = el('span', { class: 'chip fault label' }, `${failed} failed: wrong results`);
        if (valueA?.failed) lit.a = 'failed';
        if (valueB?.failed) lit.b = 'failed';
    } else if (both) {
        const result = compareValues(metric, valueA, valueB);
        if (result.winner) {
            lit[result.winner] = result.noise ? 'close' : 'won';
            verdict = el('span', { class: result.noise ? 'chip dashed label' : 'chip volt label' },
                `${names[result.winner]} ${result.size} better${result.noise ? ', may be noise' : ''}`);
        } else {
            verdict = el('span', { class: 'chip same label' }, 'The same');
        }
    } else if (valueA || valueB) {
        verdict = el('span', { class: 'chip dashed label' }, `Only ${valueA ? names.a : names.b} has run it`);
    } else {
        verdict = el('span', { class: 'chip dashed label' }, 'Not measured');
    }

    const half = (value, side) => el('div', { class: `versus-half ${side === 'a' ? 'left' : 'right'} ${lit[side]}` },
        el('span', { class: 'versus-score' }, formatValue(value?.v)),
        el('span', { class: 'versus-track' }, both ? el('span', {
            class: 'fill',
            style: `width: ${(speed(value) / top) * 100}%`,
        }) : null));

    const tags = [valueA && valueTags(valueA).map((tag) => `${names.a} ${tag}`), valueB && valueTags(valueB).map((tag) => `${names.b} ${tag}`)]
        .filter(Boolean).flat();

    return el('div', { class: 'versus-row' },
        el('span', { class: 'versus-name' },
            el('b', {}, metric.title),
            el('span', { class: 'muted' }, `${metric.unit} · ${metric.higher ? 'higher' : 'lower'} is better`),
            tags.length ? el('span', { class: 'label muted' }, tags.join(' · ')) : null),
        el('div', { class: 'versus-bars' }, half(valueA, 'a'), half(valueB, 'b')),
        el('span', { class: 'versus-verdict' }, verdict));
}

function render() {
    const a = run(state.a);
    const b = run(state.b);
    keepInAddress();
    const names = sideNames(a, b);
    $('passes').replaceChildren(renderPass('A', state.a), renderPass('B', state.b));
    /* A group neither side has run says nothing: the stress test is the usual one. */
    const groups = GROUPS.map((group, i) => ({ group, i, metrics: METRICS.filter((m) => m.group === group.id) }))
        .filter(({ metrics }) => metrics.some((m) => a.metrics[m.id] || b.metrics[m.id]));
    $('groups').replaceChildren(...groups.map(({ group, i, metrics }) => el('section', { class: 'panel versus-group' },
        el('div', { class: 'meta label muted' },
            el('span', {}, `0${i + 1} · ${group.name}`),
            el('span', {}, `${names.a} ⟵ faster ⟶ ${names.b}`)),
        ...metrics.map((metric) => renderRow(metric, a, b, names)))));
    $('copy-text').textContent = 'Copy this comparison';
}

$('swap').addEventListener('click', () => {
    [state.a, state.b] = [state.b, state.a];
    render();
});

$('copy').addEventListener('click', async () => {
    const yoursInLink = state.a === 'yours' || state.b === 'yours';
    try {
        await navigator.clipboard.writeText(location.href);
        $('copy-text').textContent = yoursInLink ? 'Copied · yours opens only here' : 'Copied';
    } catch {
        $('copy-text').textContent = 'Copy the address above';
    }
});

function firstOther(runs, than) {
    return runs.find((r) => r.id !== than)?.id;
}

loadReferences()
    .then((references) => {
        const mine = loadYours();
        state.runs = mine && rankable(mine) ? references.concat(mine) : references;
        const choices = state.runs.filter(rankable);
        if (!run(state.a) || !rankable(run(state.a))) state.a = mine && rankable(mine) ? 'yours' : choices[0].id;
        if (!run(state.b) || !rankable(run(state.b)) || state.b === state.a) state.b = firstOther(choices, state.a);
        render();
    })
    .catch(() => {
        $('groups').replaceChildren(el('p', { class: 'panel label' }, 'The results could not be loaded. Reload the page to try again.'));
    });
