// Which package the main button offers. A browser can't tell .deb from .rpm, and there's no package
// for an iPhone or iPad (which can call itself a Mac), so those go to the list.
function systemPackage() {
    const agent = navigator.userAgent;
    if (/iPhone|iPad|iPod/.test(agent) || navigator.maxTouchPoints > 1 && /Macintosh/.test(agent)) return null;
    if (/Android/.test(agent)) return { key: 'android', label: 'ANDROID ⟶' };
    if (/Windows/.test(agent)) return { key: 'windows', label: 'WINDOWS ⟶' };
    if (/Macintosh/.test(agent)) return { key: 'macos', label: 'MACOS ⟶' };
    return null;
}

function fillDownloads(downloads) {
    if (!downloads) return;
    document.querySelectorAll('[data-download]').forEach((link) => {
        link.href = downloads[link.dataset.download];
    });
    const main = document.getElementById('get');
    if (!main) return;
    const offer = systemPackage();
    main.href = offer ? downloads[offer.key] : '#platforms';
    main.querySelector('.label').textContent = offer ? offer.label : 'ALL FILES ⟶';
}

// no-cache: the browser asks again every visit, so a new release shows as soon as it's deployed.
const filled = fetch('data.json', { cache: 'no-cache' })
    .then((response) => response.json())
    .then((data) => {
        document.querySelectorAll('[data-fill]').forEach((element) => {
            const value = data[element.dataset.fill];
            if (value) element.textContent = value;
        });
        fillDownloads(data.downloads);
    })
    .catch(() => {
        // No file: the values already in the HTML stay on the page.
    });
