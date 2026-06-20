// Application Logic for NTFS Indexer Frontend
document.addEventListener('DOMContentLoaded', () => {
    // Config
    const API_BASE = ''; // Served from same origin
    let debounceTimer;

    // DOM Elements
    const elDevicePath = document.getElementById('header-device-path');
    const btnUpdateIndex = document.getElementById('btn-update-index');
    
    // Stats
    const elStatDirs = document.getElementById('stat-dirs');
    const elStatFiles = document.getElementById('stat-files');
    const elStatSize = document.getElementById('stat-size');
    const elStatUsn = document.getElementById('stat-usn');
    
    // Search
    const inputSearch = document.getElementById('search-input');
    const btnClearSearch = document.getElementById('btn-clear-search');
    const selectLimit = document.getElementById('limit-select');
    const elResultsCount = document.getElementById('results-count');
    const elSearchTime = document.getElementById('search-time');
    const tableResultsBody = document.getElementById('search-results-body');
    
    // USN Journal
    const elUsnCount = document.getElementById('usn-count');
    const btnRefreshUsn = document.getElementById('btn-refresh-usn');
    const tableUsnBody = document.getElementById('usn-results-body');
    
    // Toast Container
    const elToastContainer = document.getElementById('toast-container');

    // ----------------------------------------------------
    // Event Listeners: Tabs
    // ----------------------------------------------------
    const tabs = document.querySelectorAll('.tab-link');
    tabs.forEach(tab => {
        tab.addEventListener('click', () => {
            tabs.forEach(t => t.classList.remove('active'));
            document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
            
            tab.classList.add('active');
            const contentId = tab.getAttribute('data-tab');
            document.getElementById(contentId).classList.add('active');

            if (contentId === 'tab-usn') {
                fetchUsnJournal();
            }
        });
    });

    // ----------------------------------------------------
    // Event Listeners: Search
    // ----------------------------------------------------
    inputSearch.addEventListener('input', () => {
        if (inputSearch.value.trim() !== '') {
            btnClearSearch.style.display = 'flex';
        } else {
            btnClearSearch.style.display = 'none';
        }
        
        clearTimeout(debounceTimer);
        debounceTimer = setTimeout(performSearch, 200);
    });

    btnClearSearch.addEventListener('click', () => {
        inputSearch.value = '';
        btnClearSearch.style.display = 'none';
        performSearch();
        inputSearch.focus();
    });

    selectLimit.addEventListener('change', () => {
        performSearch();
    });

    // ----------------------------------------------------
    // Event Listeners: Actions
    // ----------------------------------------------------
    btnUpdateIndex.addEventListener('click', triggerIncrementalUpdate);
    btnRefreshUsn.addEventListener('click', fetchUsnJournal);

    // ----------------------------------------------------
    // Functions: Toast Notifications
    // ----------------------------------------------------
    function showToast(message, type = 'success', duration = 4000) {
        const toast = document.createElement('div');
        toast.className = `toast ${type}`;
        toast.innerHTML = `
            <span>${message}</span>
        `;
        elToastContainer.appendChild(toast);

        // Slide out and remove
        setTimeout(() => {
            toast.classList.add('fade-out');
            setTimeout(() => {
                toast.remove();
            }, 300);
        }, duration);
    }

    // ----------------------------------------------------
    // Functions: Utility
    // ----------------------------------------------------
    function formatBytes(bytes) {
        if (bytes === 0) return '0 B';
        if (!bytes) return '-';
        const k = 1024;
        const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
    }

    function highlightText(text, keyword) {
        if (!keyword) return text;
        const escapedKeyword = keyword.replace(/[-\/\\^$*+?.()|[\]{}]/g, '\\$&');
        const regex = new RegExp(`(${escapedKeyword})`, 'gi');
        return text.replace(regex, '<mark class="highlight">$1</mark>');
    }

    // Copy to clipboard helper
    window.copyToClipboard = function(text, btnElement) {
        navigator.clipboard.writeText(text).then(() => {
            showToast('已复制路径到剪贴板！', 'success', 2000);
            
            // Temporary icon swap feedback
            const originalHTML = btnElement.innerHTML;
            btnElement.innerHTML = `
                <svg viewBox="0 0 24 24" width="14" height="14" stroke="#10b981" stroke-width="2.5" fill="none" stroke-linecap="round" stroke-linejoin="round">
                    <polyline points="20 6 9 17 4 12"></polyline>
                </svg>
            `;
            setTimeout(() => {
                btnElement.innerHTML = originalHTML;
            }, 1500);
        }).catch(err => {
            showToast('复制失败：' + err, 'error');
        });
    };

    // ----------------------------------------------------
    // Functions: API Calls
    // ----------------------------------------------------

    // Fetch Scan Stats
    async function fetchStats() {
        // Show loading state
        elStatDirs.classList.add('skeleton');
        elStatFiles.classList.add('skeleton');
        elStatSize.classList.add('skeleton');
        elStatUsn.classList.add('skeleton');

        try {
            const res = await fetch(`${API_BASE}/api/stats`);
            if (!res.ok) throw new Error(`HTTP ${res.status}`);
            const data = await res.json();
            
            elDevicePath.textContent = data.device_path;
            elStatDirs.textContent = data.total_directories.toLocaleString();
            elStatFiles.textContent = data.total_files.toLocaleString();
            elStatSize.textContent = data.total_logical_size_formatted;
            
            // Get last USN too (we query usn state from stats or get it separate)
            // Let's fetch usn stream metadata to update the USN stat card
            fetchUsnStatCard();
        } catch (error) {
            console.error('Fetch stats error:', error);
            showToast('无法连接到后端服务，请检查后端程序是否正常运行。', 'error');
            
            elDevicePath.textContent = '未连接';
            elStatDirs.textContent = 'ERR';
            elStatFiles.textContent = 'ERR';
            elStatSize.textContent = 'ERR';
            elStatUsn.textContent = 'ERR';
        } finally {
            elStatDirs.classList.remove('skeleton');
            elStatFiles.classList.remove('skeleton');
            elStatSize.classList.remove('skeleton');
        }
    }

    async function fetchUsnStatCard() {
        try {
            const res = await fetch(`${API_BASE}/api/usn?limit=1`);
            if (!res.ok) throw new Error(`HTTP ${res.status}`);
            const data = await res.json();
            if (data.success && data.entries && data.entries.length > 0) {
                elStatUsn.textContent = data.entries[data.entries.length - 1].usn;
            } else if (data.success && data.total_records === 0) {
                elStatUsn.textContent = '0x0000000000000000';
            } else {
                elStatUsn.textContent = '未激活';
            }
        } catch (error) {
            elStatUsn.textContent = 'ERR';
        } finally {
            elStatUsn.classList.remove('skeleton');
        }
    }

    // Perform Search Query
    async function performSearch() {
        const query = inputSearch.value.trim();
        const limit = selectLimit.value;
        
        if (query === '') {
            elResultsCount.textContent = '请输入搜索关键词进行搜索';
            elSearchTime.textContent = '';
            tableResultsBody.innerHTML = `
                <tr>
                    <td colspan="4" class="table-placeholder">
                        <div class="placeholder-content">
                            <svg viewBox="0 0 24 24" width="48" height="48" stroke="currentColor" stroke-width="1.5" fill="none" stroke-linecap="round" stroke-linejoin="round">
                                <circle cx="11" cy="11" r="8"></circle>
                                <line x1="21" y1="21" x2="16.65" y2="16.65"></line>
                            </svg>
                            <p>在上方搜索框内输入任意字符，可在毫秒级获得海量 NTFS 节点结果。</p>
                        </div>
                    </td>
                </tr>
            `;
            return;
        }

        // Show table skeleton loader
        tableResultsBody.innerHTML = Array(5).fill(0).map(() => `
            <tr>
                <td><div class="skeleton" style="height: 20px; width: 80%;">Loading Path</div></td>
                <td><div class="skeleton" style="height: 20px; width: 50px;">Type</div></td>
                <td><div class="skeleton" style="height: 20px; width: 80px;">Size</div></td>
                <td style="text-align: right;"><div class="skeleton" style="height: 20px; width: 30px; display: inline-block;">Btn</div></td>
            </tr>
        `).join('');

        const startTime = performance.now();

        try {
            const url = `${API_BASE}/api/search?q=${encodeURIComponent(query)}&limit=${limit}`;
            const res = await fetch(url);
            if (!res.ok) throw new Error(`HTTP ${res.status}`);
            const data = await res.json();
            
            const elapsedMs = (performance.now() - startTime).toFixed(1);
            
            const totalCount = data.total_matches;
            elResultsCount.textContent = `找到 ${totalCount.toLocaleString()} 个匹配项` + 
                (totalCount > limit ? ` (仅显示前 ${limit} 项)` : '');
            elSearchTime.textContent = `耗时 ${elapsedMs} 毫秒`;

            if (!data.results || data.results.length === 0) {
                tableResultsBody.innerHTML = `
                    <tr>
                        <td colspan="4" class="table-placeholder">
                            没有找到任何与 "${query}" 匹配的文件或文件夹。
                        </td>
                    </tr>
                `;
                return;
            }

            tableResultsBody.innerHTML = data.results.map(item => {
                const isDir = item.is_directory;
                const typeBadge = isDir 
                    ? '<span class="type-label dir">目录</span>' 
                    : '<span class="type-label file">文件</span>';
                
                const typeIcon = isDir
                    ? `<div class="icon-badge directory">
                            <svg viewBox="0 0 24 24" width="16" height="16" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round">
                                <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"></path>
                            </svg>
                       </div>`
                    : `<div class="icon-badge file">
                            <svg viewBox="0 0 24 24" width="16" height="16" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round">
                                <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"></path>
                                <polyline points="14 2 14 8 20 8"></polyline>
                            </svg>
                       </div>`;

                const sizeStr = isDir ? '-' : formatBytes(item.size);
                
                // Escape paths and highlight keyword
                const rawPath = item.full_path;
                const safePath = rawPath.replace(/"/g, '&quot;');
                const highlightedPath = highlightText(rawPath, query);

                return `
                    <tr>
                        <td>
                            <div class="path-col">
                                ${typeIcon}
                                <span>${highlightedPath}</span>
                            </div>
                        </td>
                        <td>${typeBadge}</td>
                        <td class="size-col">${sizeStr}</td>
                        <td class="action-cell">
                            <button class="btn-copy" onclick="copyToClipboard('${safePath}', this)" title="复制完整路径">
                                <svg viewBox="0 0 24 24" width="14" height="14" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round">
                                    <rect x="9" y="9" width="13" height="13" rx="2" ry="2"></rect>
                                    <path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"></path>
                                </svg>
                            </button>
                        </td>
                    </tr>
                `;
            }).join('');

        } catch (error) {
            console.error('Search error:', error);
            tableResultsBody.innerHTML = `
                <tr>
                    <td colspan="4" class="table-placeholder" style="color: var(--color-danger)">
                        搜索出错：无法获取检索结果。请检查后端连接。
                    </td>
                </tr>
            `;
        }
    }

    // Fetch USN Change Journal Entries
    async function fetchUsnJournal() {
        elUsnCount.textContent = '正在读取 USN 变更日志数据...';
        
        tableUsnBody.innerHTML = Array(3).fill(0).map(() => `
            <tr>
                <td><div class="skeleton" style="height: 20px; width: 100%;">USN</div></td>
                <td><div class="skeleton" style="height: 20px; width: 100%;">ID</div></td>
                <td><div class="skeleton" style="height: 20px; width: 100%;">Time</div></td>
                <td><div class="skeleton" style="height: 20px; width: 100%;">Reason</div></td>
                <td><div class="skeleton" style="height: 20px; width: 100%;">Filename</div></td>
            </tr>
        `).join('');

        try {
            const res = await fetch(`${API_BASE}/api/usn?limit=100`);
            if (!res.ok) throw new Error(`HTTP ${res.status}`);
            const data = await res.json();
            
            if (!data.success) {
                elUsnCount.textContent = '变更日志不可用';
                tableUsnBody.innerHTML = `
                    <tr>
                        <td colspan="5" class="table-placeholder">
                            ${data.message || 'USN 变更日志暂未在当前分区中开启。'}
                        </td>
                    </tr>
                `;
                return;
            }

            elUsnCount.textContent = `展示最近 ${data.entries.length.toLocaleString()} 条 USN 日志变化记录`;
            
            if (!data.entries || data.entries.length === 0) {
                tableUsnBody.innerHTML = `
                    <tr>
                        <td colspan="5" class="table-placeholder">
                            USN 日志流中目前没有任何激活记录。
                        </td>
                    </tr>
                `;
                return;
            }

            // Reason badges rendering color maps
            const getReasonColorClass = (reason) => {
                if (reason.includes('CREATE')) return 'background: rgba(16, 185, 129, 0.15); color: #10b981; border: 1px solid rgba(16, 185, 129, 0.2)';
                if (reason.includes('DELETE')) return 'background: rgba(239, 68, 68, 0.15); color: #ef4444; border: 1px solid rgba(239, 68, 68, 0.2)';
                if (reason.includes('RENAME')) return 'background: rgba(245, 158, 11, 0.15); color: #f59e0b; border: 1px solid rgba(245, 158, 11, 0.2)';
                return 'background: rgba(59, 130, 246, 0.15); color: #3b82f6; border: 1px solid rgba(59, 130, 246, 0.2)';
            };

            tableUsnBody.innerHTML = data.entries.reverse().map(entry => {
                const style = getReasonColorClass(entry.reason);
                return `
                    <tr>
                        <td class="usn-col">${entry.usn}</td>
                        <td class="id-col">${entry.file_id}</td>
                        <td class="time-col">${entry.timestamp}</td>
                        <td>
                            <span class="badge-reason" style="${style}">${entry.reason}</span>
                        </td>
                        <td style="font-weight: 500;">${entry.filename}</td>
                    </tr>
                `;
            }).join('');

        } catch (error) {
            console.error('USN journal fetch error:', error);
            tableUsnBody.innerHTML = `
                <tr>
                    <td colspan="5" class="table-placeholder" style="color: var(--color-danger)">
                        读取日志出错，请检查后端运行状态。
                    </td>
                </tr>
            `;
            elUsnCount.textContent = '数据读取异常';
        }
    }

    // Trigger Incremental Update
    async function triggerIncrementalUpdate() {
        // Button load state
        const originalText = btnUpdateIndex.innerHTML;
        btnUpdateIndex.disabled = true;
        btnUpdateIndex.innerHTML = `
            <svg class="pulse" viewBox="0 0 24 24" width="16" height="16" stroke="currentColor" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round" style="animation: spin 1s linear infinite;">
                <line x1="12" y1="2" x2="12" y2="6"></line>
                <line x1="12" y1="18" x2="12" y2="22"></line>
                <line x1="4.93" y1="4.93" x2="7.76" y2="7.76"></line>
                <line x1="16.24" y1="16.24" x2="19.07" y2="19.07"></line>
                <line x1="2" y1="12" x2="6" y2="12"></line>
                <line x1="18" y1="12" x2="22" y2="12"></line>
                <line x1="4.93" y1="19.07" x2="7.76" y2="16.24"></line>
                <line x1="16.24" y1="7.76" x2="19.07" y2="4.93"></line>
            </svg>
            <span>增量扫描中...</span>
        `;

        showToast('开始增量扫描 USN Change Journal...', 'info', 2000);

        try {
            const res = await fetch(`${API_BASE}/api/update`, {
                method: 'POST'
            });
            if (!res.ok) throw new Error(`HTTP ${res.status}`);
            const data = await res.json();
            
            if (data.success) {
                showToast(`增量更新成功！当前位置 ${data.last_usn}，耗时 ${data.elapsed_ms} 毫秒。`, 'success');
                
                // Refresh data
                await fetchStats();
                
                // If search is active, refresh search
                if (inputSearch.value.trim() !== '') {
                    performSearch();
                }
                
                // If USN tab is active, refresh journal logs
                const usnTabActive = document.querySelector('.tab-link[data-tab="tab-usn"]').classList.contains('active');
                if (usnTabActive) {
                    fetchUsnJournal();
                }
            } else {
                showToast('增量更新失败：' + (data.message || '原因未知'), 'error');
            }
        } catch (error) {
            console.error('Update index error:', error);
            showToast('请求接口失败：请检查网络或后端是否掉线。', 'error');
        } finally {
            btnUpdateIndex.disabled = false;
            btnUpdateIndex.innerHTML = originalText;
        }
    }

    // CSS Keyframe Injection for Rotation in Button
    const styleSheet = document.createElement("style");
    styleSheet.innerText = `
        @keyframes spin {
            from { transform: rotate(0deg); }
            to { transform: rotate(360deg); }
        }
        .highlight {
            background-color: rgba(99, 102, 241, 0.35);
            color: #ffffff;
            padding: 2px 4px;
            border-radius: 4px;
            font-weight: 600;
        }
    `;
    document.head.appendChild(styleSheet);

    // ----------------------------------------------------
    // Startup initialization
    // ----------------------------------------------------
    fetchStats();
});
