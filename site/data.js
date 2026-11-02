const filled = fetch('data.json')
    .then((response) => response.json())
    .then((data) => {
        document.querySelectorAll('[data-fill]').forEach((element) => {
            const value = data[element.dataset.fill];
            if (value) element.textContent = value;
        });
    })
    .catch(() => {
        // No file: the values already in the HTML stay on the page.
    });
