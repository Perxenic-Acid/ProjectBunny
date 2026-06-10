<script setup lang="ts">
import { computed, ref } from "vue";
import { Search } from "@element-plus/icons-vue";

const props = withDefaults(
  defineProps<{
    title?: string;
    columns: string[];
    rows: unknown[][];
    height?: number;
    pageSize?: number;
    dense?: boolean;
  }>(),
  {
    title: "",
    height: 520,
    pageSize: 50,
    dense: false,
  },
);

const query = ref("");
const page = ref(1);

const filteredRows = computed(() => {
  const text = query.value.trim().toLowerCase();
  if (!text) {
    return props.rows;
  }

  return props.rows.filter((row) =>
    row.some((value) => String(value ?? "").toLowerCase().includes(text)),
  );
});

const pageRows = computed(() => {
  const start = (page.value - 1) * props.pageSize;
  return filteredRows.value.slice(start, start + props.pageSize).map((row, rowIndex) => {
    const record: Record<string, unknown> = {
      __rowId: `${page.value}-${rowIndex}`,
    };

    props.columns.forEach((column, index) => {
      record[column] = row[index] ?? "";
    });

    return record;
  });
});

const columnWidth = (column: string) => {
  if (["tags", "shader_tags", "resource_refs", "graphics_tables", "compute_tables", "file", "asm_file", "relativePath"].includes(column)) {
    return 260;
  }

  if (column.includes("hash") || column.includes("signature") || column.includes("resource")) {
    return 190;
  }

  if (column.includes("handle") || column.includes("pipeline") || column.includes("heap")) {
    return 190;
  }

  if (column.length > 18) {
    return 170;
  }

  return 130;
};
</script>

<template>
  <section class="data-table">
    <div class="table-toolbar">
      <div>
        <h3 v-if="title">{{ title }}</h3>
        <span>{{ filteredRows.length.toLocaleString() }} / {{ rows.length.toLocaleString() }} rows</span>
      </div>
      <el-input
        v-model="query"
        :prefix-icon="Search"
        clearable
        placeholder="Filter rows"
        class="table-search"
        @input="page = 1"
      />
    </div>

    <el-table
      :data="pageRows"
      :height="height"
      :row-key="(row: any) => row.__rowId"
      :size="dense ? 'small' : 'default'"
      border
      stripe
    >
      <el-table-column
        v-for="column in columns"
        :key="column"
        :label="column"
        :min-width="columnWidth(column)"
        show-overflow-tooltip
      >
        <template #default="{ row }">
          <span class="cell-value">{{ row[column] }}</span>
        </template>
      </el-table-column>
    </el-table>

    <div class="pagination-row">
      <el-pagination
        v-model:current-page="page"
        :page-size="pageSize"
        :total="filteredRows.length"
        background
        layout="prev, pager, next"
      />
    </div>
  </section>
</template>

<style scoped>
.data-table {
  min-width: 0;
}

.table-toolbar {
  align-items: center;
  display: flex;
  gap: 16px;
  justify-content: space-between;
  margin-bottom: 12px;
}

.table-toolbar h3 {
  color: #172033;
  font-size: 16px;
  line-height: 20px;
  margin: 0 0 2px;
}

.table-toolbar span {
  color: #667085;
  font-size: 12px;
}

.table-search {
  max-width: 320px;
}

.cell-value {
  font-family: "JetBrains Mono", "SFMono-Regular", Consolas, monospace;
  font-size: 12px;
}

.pagination-row {
  display: flex;
  justify-content: flex-end;
  padding-top: 12px;
}

@media (max-width: 760px) {
  .table-toolbar {
    align-items: stretch;
    flex-direction: column;
  }

  .table-search {
    max-width: none;
  }
}
</style>
