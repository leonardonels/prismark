// The phone wallet needs each ticket's height: see "Phone" at the end of styles.css.
const tickets = new ResizeObserver((entries) => {
    entries.forEach((entry) => {
        entry.target.style.setProperty('--h', entry.target.offsetHeight + 'px');
    });
});

document.querySelectorAll('.deck > .panel').forEach((panel) => tickets.observe(panel));

// Each animation waits, paused, until its element is on screen, and plays once.
const watcher = new IntersectionObserver((entries) => {
    entries.forEach((entry) => {
        if (entry.isIntersecting) {
            entry.target.classList.add('run');
            watcher.unobserve(entry.target);
        }
    });
}, { threshold: 0.4 });

watcher.observe(document.getElementById('curve'));

// A number rests on its real value and rolls back to it, so it's never wrong.
function prepareRoll(element) {
    const text = element.textContent;
    element.textContent = '';
    element.setAttribute('role', 'img');
    element.setAttribute('aria-label', text);
    element.classList.add('rolling');

    [...text].forEach((character, position) => {
        const cell = document.createElement('span');
        const reel = document.createElement('span');
        const count = /\d/.test(character) ? 4 + position * 2 : 0;

        for (let k = 0; k <= count; k++) {
            const line = document.createElement('span');
            line.style.setProperty('--k', k);
            line.textContent = k === 0 || k === count ? character : '72941683'[(k + position * 3) % 8];
            reel.appendChild(line);
        }

        cell.className = 'roll-cell';
        reel.className = 'roll-reel';
        reel.style.setProperty('--n', position);
        reel.style.setProperty('--count', count);
        cell.appendChild(reel);
        element.appendChild(cell);
    });

    watcher.observe(element);
}

// Wait for data.js, because filling a value would replace the reels.
if (!matchMedia('(prefers-reduced-motion: reduce)').matches) {
    filled.then(() => document.querySelectorAll('.score').forEach(prepareRoll));
}

// A download link lands on the version, so it rolls again every time.
document.querySelectorAll('a[href="#download"]').forEach((link) => {
    link.addEventListener('click', () => {
        const version = document.querySelector('.download .score');
        version.classList.add('run');
        version.querySelectorAll('.roll-reel').forEach((reel) => {
            reel.getAnimations().forEach((animation) => {
                animation.cancel();
                animation.play();
            });
        });
    });
});

// On a phone, tapping a closed ticket's strip reopens that ticket.
function restingScroll(ticket) {
    const panels = [...document.querySelectorAll('.deck > .panel')].filter((panel) => panel.offsetHeight);
    let top = document.getElementById('top').getBoundingClientRect().top + window.scrollY;

    for (const panel of panels) {
        if (panel === ticket) break;
        top += panel.offsetHeight + parseFloat(getComputedStyle(document.querySelector('.ground')).rowGap);
    }

    return top - parseFloat(getComputedStyle(ticket).top);
}

document.addEventListener('click', (event) => {
    const ticket = event.target.closest('.deck > .panel');
    if (!ticket || getComputedStyle(ticket).position !== 'sticky') return;

    const panels = [...document.querySelectorAll('.deck > .panel')].filter((panel) => panel.offsetHeight);
    const next = panels[panels.indexOf(ticket) + 1];
    const closed = next && next.getBoundingClientRect().top - ticket.getBoundingClientRect().top <= 40;
    if (!closed) return;

    event.preventDefault();
    event.stopPropagation();
    window.scrollTo({ top: restingScroll(ticket), behavior: 'smooth' });
}, true);
